#include "repository/repository.h"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <winioctl.h>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace {
constexpr char kConfigMagic[] = "DBR1";
constexpr char kManifestMagic[] = "DBM1";
constexpr uint32_t kPbkdfRounds = 100000;

struct ManifestEntry {
    uint8_t type = 0;  // 0 file, 1 directory, 2 reparse point
    std::string path;
    uint32_t attributes = 0;
    int64_t mtimeMs = 0;
    uint64_t size = 0;
    std::string linkTarget;
    std::vector<uint8_t> security;
    std::vector<std::string> chunks;
};

struct Manifest { SnapshotInfo info; std::vector<ManifestEntry> entries; };

struct ReparseBuffer {
    DWORD tag; USHORT dataLength; USHORT reserved;
    union {
        struct { USHORT substituteOffset, substituteLength, printOffset, printLength; ULONG flags; WCHAR path[1]; } symlink;
        struct { USHORT substituteOffset, substituteLength, printOffset, printLength; WCHAR path[1]; } mount;
        struct { UCHAR data[1]; } generic;
    } value;
};

std::wstring toWide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n);
    return result;
}

std::string toUtf8(const std::wstring &s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), n, nullptr, nullptr);
    return result;
}

bool makeDirs(const std::string &path) {
    std::wstring value = toWide(path);
    for (size_t i = 1; i < value.size(); ++i) if (value[i] == L'/' || value[i] == L'\\') {
        wchar_t saved = value[i]; value[i] = 0; CreateDirectoryW(value.c_str(), nullptr); value[i] = saved;
    }
    return CreateDirectoryW(value.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool randomBytes(std::vector<uint8_t> &data, size_t size) {
    data.resize(size);
    return BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

bool sha256(const uint8_t *data, size_t size, std::vector<uint8_t> &digest) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0; std::vector<uint8_t> object;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) >= 0;
    object.resize(objectSize); digest.resize(32);
    ok = ok && BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0;
    ok = ok && BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0;
    ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); return ok;
}

std::string hex(const std::vector<uint8_t> &bytes) {
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (uint8_t value : bytes) out << std::setw(2) << static_cast<unsigned>(value);
    return out.str();
}

bool deriveKey(const std::string &password, const std::vector<uint8_t> &salt, std::vector<uint8_t> &key) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; key.resize(32);
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return false;
    NTSTATUS status = BCryptDeriveKeyPBKDF2(algorithm,
        reinterpret_cast<PUCHAR>(const_cast<char *>(password.data())), static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()), kPbkdfRounds,
        key.data(), static_cast<ULONG>(key.size()), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0); return status >= 0;
}

bool aesGcm(bool encrypt, const std::vector<uint8_t> &input, const std::vector<uint8_t> &key,
            std::vector<uint8_t> &nonce, std::vector<uint8_t> &tag, std::vector<uint8_t> &output) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_KEY_HANDLE keyHandle = nullptr;
    DWORD objectSize = 0, got = 0, outputSize = 0; std::vector<uint8_t> object;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
    bool ok = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0) >= 0;
    ok = ok && BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &got, 0) >= 0;
    object.resize(objectSize);
    ok = ok && BCryptGenerateSymmetricKey(algorithm, &keyHandle, object.data(), objectSize, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) >= 0;
    if (encrypt && nonce.empty()) ok = ok && randomBytes(nonce, 12);
    if (encrypt) tag.assign(16, 0);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth; BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = nonce.data(); auth.cbNonce = static_cast<ULONG>(nonce.size()); auth.pbTag = tag.data(); auth.cbTag = static_cast<ULONG>(tag.size());
    if (ok) {
        NTSTATUS status = encrypt
            ? BCryptEncrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), &auth, nullptr, 0, nullptr, 0, &outputSize, 0)
            : BCryptDecrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), &auth, nullptr, 0, nullptr, 0, &outputSize, 0);
        ok = status >= 0; output.resize(outputSize);
    }
    if (ok) {
        NTSTATUS status = encrypt
            ? BCryptEncrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), &auth, nullptr, 0, output.data(), outputSize, &got, 0)
            : BCryptDecrypt(keyHandle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), &auth, nullptr, 0, output.data(), outputSize, &got, 0);
        ok = status >= 0; output.resize(got);
    }
    if (keyHandle) BCryptDestroyKey(keyHandle); BCryptCloseAlgorithmProvider(algorithm, 0); return ok;
}

void putU32(std::vector<uint8_t> &out, uint32_t v) { for (int i=0;i<4;++i) out.push_back(static_cast<uint8_t>(v>>(i*8))); }
void putU64(std::vector<uint8_t> &out, uint64_t v) { for (int i=0;i<8;++i) out.push_back(static_cast<uint8_t>(v>>(i*8))); }
void putString(std::vector<uint8_t> &out, const std::string &s) { putU32(out, static_cast<uint32_t>(s.size())); out.insert(out.end(), s.begin(), s.end()); }
void putBytes(std::vector<uint8_t> &out, const std::vector<uint8_t> &v) { putU32(out, static_cast<uint32_t>(v.size())); out.insert(out.end(), v.begin(), v.end()); }
bool take(const std::vector<uint8_t>&in,size_t&p,void*d,size_t n){if(p>in.size()||n>in.size()-p)return false;memcpy(d,in.data()+p,n);p+=n;return true;}
bool getU32(const std::vector<uint8_t>&in,size_t&p,uint32_t&v){uint8_t b[4];if(!take(in,p,b,4))return false;v=b[0]|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);return true;}
bool getU64(const std::vector<uint8_t>&in,size_t&p,uint64_t&v){uint8_t b[8];if(!take(in,p,b,8))return false;v=0;for(int i=0;i<8;++i)v|=uint64_t(b[i])<<(i*8);return true;}
bool getString(const std::vector<uint8_t>&in,size_t&p,std::string&s){uint32_t n;if(!getU32(in,p,n)||n>in.size()-p)return false;s.assign(reinterpret_cast<const char*>(in.data()+p),n);p+=n;return true;}
bool getBytes(const std::vector<uint8_t>&in,size_t&p,std::vector<uint8_t>&v){uint32_t n;if(!getU32(in,p,n)||n>in.size()-p)return false;v.assign(in.begin()+p,in.begin()+p+n);p+=n;return true;}

bool readFile(const std::string &path, std::vector<uint8_t> &data) { std::ifstream f(std::filesystem::path(toWide(path)),std::ios::binary); if(!f)return false; data.assign(std::istreambuf_iterator<char>(f),{}); return !f.bad(); }
bool writeAtomic(const std::string &path,const std::vector<uint8_t>&data){std::string temp=path+".tmp";std::ofstream f(std::filesystem::path(toWide(temp)),std::ios::binary|std::ios::trunc);if(!f)return false;f.write(reinterpret_cast<const char*>(data.data()),data.size());f.close();if(!f)return false;return MoveFileExW(toWide(temp).c_str(),toWide(path).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;}

int64_t fileTimeMs(const FILETIME &ft){ULARGE_INTEGER v;v.LowPart=ft.dwLowDateTime;v.HighPart=ft.dwHighDateTime;return (static_cast<int64_t>(v.QuadPart)-116444736000000000LL)/10000;}
FILETIME toFileTime(int64_t ms){ULARGE_INTEGER v;v.QuadPart=static_cast<uint64_t>(ms*10000+116444736000000000LL);return {v.LowPart,v.HighPart};}

std::vector<uint8_t> securityDescriptor(const std::wstring &path) {
    DWORD size=0; GetFileSecurityW(path.c_str(),OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,0,&size);
    std::vector<uint8_t> value(size); if(size&&!GetFileSecurityW(path.c_str(),OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,reinterpret_cast<PSECURITY_DESCRIPTOR>(value.data()),size,&size))value.clear(); return value;
}

bool matches(const WIN32_FIND_DATAW &data,const std::string &relative,const FilterOptions &filter){
    bool dir=(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;std::string lower=relative;std::transform(lower.begin(),lower.end(),lower.begin(),::tolower);
    std::string name=toUtf8(data.cFileName);std::transform(name.begin(),name.end(),name.begin(),::tolower);
    if(filter.type==EntryTypeFilter::FilesOnly&&dir)return false;if(filter.type==EntryTypeFilter::DirectoriesOnly&&!dir)return false;
    if(!filter.pathContains.empty()&&lower.find(filter.pathContains)==std::string::npos)return false;
    if(!filter.nameContains.empty()&&name.find(filter.nameContains)==std::string::npos)return false;
    if(!dir&&!filter.extensions.empty()){bool ok=false;for(auto ext:filter.extensions){std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);if(!ext.empty()&&ext[0]!='.')ext="."+ext;if(name.size()>=ext.size()&&name.substr(name.size()-ext.size())==ext)ok=true;}if(!ok)return false;}
    uint64_t size=(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;if(filter.minSize&&size<filter.minSize)return false;if(filter.maxSize&&size>filter.maxSize)return false;
    int64_t time=fileTimeMs(data.ftLastWriteTime);if(filter.afterMtimeMs&&time<filter.afterMtimeMs)return false;if(filter.beforeMtimeMs&&time>filter.beforeMtimeMs)return false;return true;
}

bool serialize(const Manifest&m,std::vector<uint8_t>&out){out.assign(kManifestMagic,kManifestMagic+4);putString(out,m.info.id);putU64(out,m.info.createdAtMs);putU64(out,m.info.fileCount);putU64(out,m.info.logicalBytes);putU64(out,m.info.storedBytes);putU32(out,m.entries.size());for(auto&e:m.entries){out.push_back(e.type);putString(out,e.path);putU32(out,e.attributes);putU64(out,e.mtimeMs);putU64(out,e.size);putString(out,e.linkTarget);putBytes(out,e.security);putU32(out,e.chunks.size());for(auto&c:e.chunks)putString(out,c);}return true;}
bool deserialize(const std::vector<uint8_t>&in,Manifest&m){if(in.size()<4||memcmp(in.data(),kManifestMagic,4))return false;size_t p=4;uint64_t t;uint32_t count;if(!getString(in,p,m.info.id)||!getU64(in,p,t))return false;m.info.createdAtMs=static_cast<int64_t>(t);if(!getU64(in,p,m.info.fileCount)||!getU64(in,p,m.info.logicalBytes)||!getU64(in,p,m.info.storedBytes)||!getU32(in,p,count)||count>10000000)return false;for(uint32_t i=0;i<count;++i){ManifestEntry e;if(!take(in,p,&e.type,1)||!getString(in,p,e.path)||!getU32(in,p,e.attributes)||!getU64(in,p,t))return false;e.mtimeMs=static_cast<int64_t>(t);uint32_t n;if(!getU64(in,p,e.size)||!getString(in,p,e.linkTarget)||!getBytes(in,p,e.security)||!getU32(in,p,n)||n>1000000)return false;for(uint32_t j=0;j<n;++j){std::string c;if(!getString(in,p,c))return false;e.chunks.push_back(c);}m.entries.push_back(std::move(e));}return p==in.size();}

std::string baseName(const std::string&p){size_t pos=p.find_last_of("/\\");return pos==std::string::npos?p:p.substr(pos+1);}
std::string parentPath(const std::string&p){size_t pos=p.find_last_of("/\\");return pos==std::string::npos?std::string{}:p.substr(0,pos);}

std::string reparseTarget(const std::string &path) {
    HANDLE handle=CreateFileW(toWide(path).c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return{};std::vector<uint8_t>data(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);DWORD got=0;
    bool ok=DeviceIoControl(handle,FSCTL_GET_REPARSE_POINT,nullptr,0,data.data(),data.size(),&got,nullptr)!=0;CloseHandle(handle);if(!ok)return{};
    auto*buffer=reinterpret_cast<ReparseBuffer*>(data.data());const WCHAR*text=nullptr;USHORT offset=0,length=0;
    if(buffer->tag==IO_REPARSE_TAG_SYMLINK){text=buffer->value.symlink.path;offset=buffer->value.symlink.printOffset;length=buffer->value.symlink.printLength;}
    else if(buffer->tag==IO_REPARSE_TAG_MOUNT_POINT){text=buffer->value.mount.path;offset=buffer->value.mount.printOffset;length=buffer->value.mount.printLength;}else return{};
    return toUtf8(std::wstring(reinterpret_cast<const WCHAR*>(reinterpret_cast<const uint8_t*>(text)+offset),length/sizeof(WCHAR)));
}

bool scanDirectory(const std::string&dir,const std::string&base,const SnapshotOptions&options,Manifest&m,const std::string&root,const std::vector<uint8_t>&key,std::set<std::string>&newChunks,std::string&error,const OperationContext&ctx){
    WIN32_FIND_DATAW data;HANDLE find=FindFirstFileW((toWide(dir)+L"\\*").c_str(),&data);if(find==INVALID_HANDLE_VALUE){error="无法扫描目录: "+dir;return false;}
    do{if(wcscmp(data.cFileName,L".")==0||wcscmp(data.cFileName,L"..")==0)continue;if(ctx.isCancelled()){error="操作已取消";FindClose(find);return false;}
        std::string name=toUtf8(data.cFileName),path=dir+"\\"+name,relative=base.empty()?name:base+"/"+name;bool dirFlag=(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        ManifestEntry e;e.path=relative;e.attributes=data.dwFileAttributes;e.mtimeMs=fileTimeMs(data.ftLastWriteTime);e.security=securityDescriptor(toWide(path));
        if(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){e.type=2;e.linkTarget=reparseTarget(path);m.entries.push_back(std::move(e));continue;}
        bool include=matches(data,relative,options.filter);if(dirFlag){e.type=1;if(include)m.entries.push_back(e);if(!scanDirectory(path,relative,options,m,root,key,newChunks,error,ctx)){FindClose(find);return false;}continue;}if(!include)continue;
        e.type=0;e.size=(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;std::ifstream input(std::filesystem::path(toWide(path)),std::ios::binary);if(!input){error="无法读取文件: "+path;FindClose(find);return false;}std::vector<uint8_t> buffer(options.chunkSize);while(input){input.read(reinterpret_cast<char*>(buffer.data()),buffer.size());std::streamsize got=input.gcount();if(got<=0)break;std::vector<uint8_t> digest;if(!sha256(buffer.data(),got,digest)){error="SHA-256 失败";FindClose(find);return false;}std::string id=hex(digest);e.chunks.push_back(id);std::string chunkDir=root+"\\chunks\\"+id.substr(0,2);std::string chunkPath=chunkDir+"\\"+id+".blk";DWORD attr=GetFileAttributesW(toWide(chunkPath).c_str());if(attr==INVALID_FILE_ATTRIBUTES){makeDirs(chunkDir);std::vector<uint8_t> plain(buffer.begin(),buffer.begin()+got),nonce,tag,cipher;if(!aesGcm(true,plain,key,nonce,tag,cipher)){error="AES-GCM 加密失败";FindClose(find);return false;}std::vector<uint8_t> stored={'D','B','C','1'};stored.insert(stored.end(),nonce.begin(),nonce.end());stored.insert(stored.end(),tag.begin(),tag.end());stored.insert(stored.end(),cipher.begin(),cipher.end());if(!writeAtomic(chunkPath,stored)){error="无法写入数据块";FindClose(find);return false;}newChunks.insert(id);m.info.storedBytes+=stored.size();}}
        m.info.fileCount++;m.info.logicalBytes+=e.size;m.entries.push_back(std::move(e));OperationProgress progress;progress.stage=OperationStage::Reading;progress.currentPath=relative;progress.completedFiles=m.info.fileCount;progress.completedBytes=m.info.logicalBytes;ctx.report(progress);
    }while(FindNextFileW(find,&data));FindClose(find);return true;
}

bool scanSelectedFile(const std::string &path,const SnapshotOptions&o,Manifest&m,const std::string&root,const std::vector<uint8_t>&key,std::set<std::string>&newChunks,std::string&error,const OperationContext&ctx){WIN32_FIND_DATAW data;HANDLE find=FindFirstFileW(toWide(path).c_str(),&data);if(find==INVALID_HANDLE_VALUE){error="无法读取来源: "+path;return false;}FindClose(find);std::string relative=baseName(path);if(!matches(data,relative,o.filter))return true;ManifestEntry e;e.path=relative;e.attributes=data.dwFileAttributes;e.mtimeMs=fileTimeMs(data.ftLastWriteTime);e.security=securityDescriptor(toWide(path));if(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){e.type=2;e.linkTarget=reparseTarget(path);m.entries.push_back(std::move(e));return true;}e.size=(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;std::ifstream input(std::filesystem::path(toWide(path)),std::ios::binary);if(!input){error="无法读取文件: "+path;return false;}std::vector<uint8_t>buffer(o.chunkSize);while(input){if(ctx.isCancelled()){error="操作已取消";return false;}input.read(reinterpret_cast<char*>(buffer.data()),buffer.size());std::streamsize got=input.gcount();if(got<=0)break;std::vector<uint8_t>digest;if(!sha256(buffer.data(),got,digest)){error="SHA-256 失败";return false;}std::string id=hex(digest);e.chunks.push_back(id);std::string dir=root+"\\chunks\\"+id.substr(0,2),chunkPath=dir+"\\"+id+".blk";if(GetFileAttributesW(toWide(chunkPath).c_str())==INVALID_FILE_ATTRIBUTES){makeDirs(dir);std::vector<uint8_t>plain(buffer.begin(),buffer.begin()+got),nonce,tag,cipher;if(!aesGcm(true,plain,key,nonce,tag,cipher)){error="AES-GCM 加密失败";return false;}std::vector<uint8_t>stored={'D','B','C','1'};stored.insert(stored.end(),nonce.begin(),nonce.end());stored.insert(stored.end(),tag.begin(),tag.end());stored.insert(stored.end(),cipher.begin(),cipher.end());if(!writeAtomic(chunkPath,stored)){error="无法写入数据块";return false;}newChunks.insert(id);m.info.storedBytes+=stored.size();}}m.info.fileCount++;m.info.logicalBytes+=e.size;m.entries.push_back(std::move(e));return true;}

bool loadConfig(const std::string&root,std::vector<uint8_t>&salt){std::vector<uint8_t>d;if(!readFile(root+"\\config.bin",d)||d.size()!=20||memcmp(d.data(),kConfigMagic,4))return false;salt.assign(d.begin()+4,d.end());return true;}
bool loadManifest(const std::string&root,const std::string&id,const std::vector<uint8_t>&key,Manifest&m){std::vector<uint8_t>d;if(!readFile(root+"\\snapshots\\"+id+".manifest",d)||d.size()<32||memcmp(d.data(),"DBE1",4))return false;std::vector<uint8_t>nonce(d.begin()+4,d.begin()+16),tag(d.begin()+16,d.begin()+32),cipher(d.begin()+32,d.end()),plain;if(!aesGcm(false,cipher,key,nonce,tag,plain))return false;return deserialize(plain,m);}
}

LocalRepository::LocalRepository(std::string root):root_(std::move(root)){}

bool LocalRepository::initialize(const std::string&password,std::string&error){activePassword_=password;if(!makeDirs(root_+"\\chunks")||!makeDirs(root_+"\\snapshots")){error="无法创建仓库目录";return false;}if(loadConfig(root_,salt_))return true;if(!randomBytes(salt_,16)){error="无法生成仓库随机盐";return false;}std::vector<uint8_t>d(kConfigMagic,kConfigMagic+4);d.insert(d.end(),salt_.begin(),salt_.end());if(!writeAtomic(root_+"\\config.bin",d)){error="无法写入仓库配置";return false;}return true;}

bool LocalRepository::createSnapshot(const SnapshotOptions&o,SnapshotInfo&s,std::string&error,const OperationContext&ctx){if(o.sources.empty()||o.password.empty()){error="增量仓库需要来源和密码";return false;}if(!initialize(o.password,error))return false;std::vector<uint8_t>key;if(!deriveKey(o.password,salt_,key)){error="密钥派生失败";return false;}Manifest m;m.info.createdAtMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();std::vector<uint8_t>random;if(!randomBytes(random,8)){error="无法生成快照 ID";return false;}m.info.id=std::to_string(m.info.createdAtMs)+"-"+hex(random);std::set<std::string>newChunks;
    for(const auto&source:o.sources){DWORD attr=GetFileAttributesW(toWide(source).c_str());if(attr==INVALID_FILE_ATTRIBUTES){error="来源不存在: "+source;return false;}if(attr&FILE_ATTRIBUTE_DIRECTORY){if(!scanDirectory(source,baseName(source),o,m,root_,key,newChunks,error,ctx))return false;}else if(!scanSelectedFile(source,o,m,root_,key,newChunks,error,ctx))return false;}
    std::vector<uint8_t>plain;serialize(m,plain);std::vector<uint8_t>nonce,tag,cipher;if(!aesGcm(true,plain,key,nonce,tag,cipher)){error="清单加密失败";return false;}std::vector<uint8_t>stored={'D','B','E','1'};stored.insert(stored.end(),nonce.begin(),nonce.end());stored.insert(stored.end(),tag.begin(),tag.end());stored.insert(stored.end(),cipher.begin(),cipher.end());if(!writeAtomic(root_+"\\snapshots\\"+m.info.id+".manifest",stored)){error="快照清单提交失败";return false;}s=m.info;OperationProgress done;done.stage=OperationStage::Completed;done.completedFiles=s.fileCount;done.completedBytes=s.logicalBytes;ctx.report(done);return true;}

bool LocalRepository::listSnapshots(std::vector<SnapshotInfo>&out,std::string&error){out.clear();if(salt_.empty()&&!loadConfig(root_,salt_)){error="仓库未初始化";return false;}WIN32_FIND_DATAW d;HANDLE h=FindFirstFileW((toWide(root_)+L"\\snapshots\\*.manifest").c_str(),&d);if(h==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;do{std::string file=toUtf8(d.cFileName);SnapshotInfo s;s.id=file.substr(0,file.size()-9);size_t dash=s.id.find('-');if(dash!=std::string::npos)try{s.createdAtMs=std::stoll(s.id.substr(0,dash));}catch(...){}out.push_back(s);}while(FindNextFileW(h,&d));FindClose(h);std::sort(out.begin(),out.end(),[](auto&a,auto&b){return a.createdAtMs>b.createdAtMs;});return true;}

bool LocalRepository::restoreSnapshot(const std::string&id,const std::string&destination,const std::string&password,std::string&error,const OperationContext&ctx){if(salt_.empty()&&!loadConfig(root_,salt_)){error="仓库配置无效";return false;}std::vector<uint8_t>key;if(!deriveKey(password,salt_,key)){error="密钥派生失败";return false;}Manifest m;if(!loadManifest(root_,id,key,m)){error="密码错误或快照清单损坏";return false;}makeDirs(destination);uint64_t completed=0;for(const auto&e:m.entries){if(ctx.isCancelled()){error="操作已取消";return false;}if(e.path.empty()||e.path.find("..")!=std::string::npos||e.path.find(':')!=std::string::npos){error="快照包含不安全路径";return false;}std::string path=destination+"\\"+e.path;if(e.type==1){makeDirs(path);continue;}if(e.type==2){std::wstring target=toWide(e.linkTarget);if(target.empty()||target[0]==L'/'||target[0]==L'\\'||target.find(L':')!=std::wstring::npos||e.linkTarget.find("..")!=std::string::npos){error="拒绝恢复不安全的链接目标";return false;}makeDirs(parentPath(path));DWORD flags=(e.attributes&FILE_ATTRIBUTE_DIRECTORY)?SYMBOLIC_LINK_FLAG_DIRECTORY:0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
flags|=SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
if(!CreateSymbolicLinkW(toWide(path).c_str(),target.c_str(),flags)){error="无法恢复符号链接: "+e.path;return false;}continue;}makeDirs(parentPath(path));std::string temp=path+".tmp.restore";std::ofstream output(temp,std::ios::binary|std::ios::trunc);if(!output){error="无法创建恢复文件";return false;}for(const auto&chunk:e.chunks){std::vector<uint8_t>d;if(!readFile(root_+"\\chunks\\"+chunk.substr(0,2)+"\\"+chunk+".blk",d)||d.size()<32||memcmp(d.data(),"DBC1",4)){error="数据块缺失或损坏: "+chunk;return false;}std::vector<uint8_t>nonce(d.begin()+4,d.begin()+16),tag(d.begin()+16,d.begin()+32),cipher(d.begin()+32,d.end()),plain;if(!aesGcm(false,cipher,key,nonce,tag,plain)){error="密码错误或数据块认证失败";return false;}std::vector<uint8_t>digest;sha256(plain.data(),plain.size(),digest);if(hex(digest)!=chunk){error="数据块哈希不匹配";return false;}output.write(reinterpret_cast<const char*>(plain.data()),plain.size());}output.close();if(!MoveFileExW(toWide(temp).c_str(),toWide(path).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){error="无法提交恢复文件";return false;}FILETIME ft=toFileTime(e.mtimeMs);HANDLE fh=CreateFileW(toWide(path).c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(fh!=INVALID_HANDLE_VALUE){SetFileTime(fh,nullptr,nullptr,&ft);CloseHandle(fh);}SetFileAttributesW(toWide(path).c_str(),e.attributes);if(!e.security.empty())SetFileSecurityW(toWide(path).c_str(),OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,reinterpret_cast<PSECURITY_DESCRIPTOR>(const_cast<uint8_t*>(e.security.data())));OperationProgress p;p.stage=OperationStage::Restoring;p.currentPath=e.path;p.completedFiles=++completed;p.totalFiles=m.info.fileCount;ctx.report(p);}return true;}

bool LocalRepository::deleteSnapshot(const std::string&id,std::string&error){if(!DeleteFileW(toWide(root_+"\\snapshots\\"+id+".manifest").c_str())){error="无法删除快照";return false;}return true;}
bool LocalRepository::prune(size_t keep,std::string&error){std::vector<SnapshotInfo>all;if(!listSnapshots(all,error))return false;for(size_t i=keep;i<all.size();++i)if(!deleteSnapshot(all[i].id,error))return false;if(activePassword_.empty())return true;std::vector<uint8_t>key;if(!deriveKey(activePassword_,salt_,key))return false;std::set<std::string>live;for(size_t i=0;i<std::min(keep,all.size());++i){Manifest m;if(!loadManifest(root_,all[i].id,key,m))continue;for(auto&e:m.entries)for(auto&chunk:e.chunks)live.insert(chunk);}WIN32_FIND_DATAW dir;HANDLE dh=FindFirstFileW((toWide(root_)+L"\\chunks\\*").c_str(),&dir);if(dh!=INVALID_HANDLE_VALUE){do{if(!(dir.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||dir.cFileName[0]==L'.')continue;std::string folder=toUtf8(dir.cFileName);WIN32_FIND_DATAW file;HANDLE fh=FindFirstFileW((toWide(root_+"\\chunks\\"+folder)+L"\\*.blk").c_str(),&file);if(fh!=INVALID_HANDLE_VALUE){do{std::string name=toUtf8(file.cFileName),id=name.substr(0,name.size()-4);if(!live.count(id))DeleteFileW(toWide(root_+"\\chunks\\"+folder+"\\"+name).c_str());}while(FindNextFileW(fh,&file));FindClose(fh);}}while(FindNextFileW(dh,&dir));FindClose(dh);}return true;}
