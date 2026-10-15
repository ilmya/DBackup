#include "repository/repository.h"
#include "path_safety.h"
#include "core/crypto.h"

#include <windows.h>
#include <aclapi.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <winioctl.h>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <cwctype>

namespace {
using dbackup::crypto::randomBytes;
using dbackup::crypto::sha256;

constexpr char kConfigMagic[] = "DBR1";
constexpr char kManifestMagic[] = "DBM1";

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

std::string hex(const std::vector<uint8_t> &bytes) {
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (uint8_t value : bytes) out << std::setw(2) << static_cast<unsigned>(value);
    return out.str();
}

bool deriveKey(const std::string &password, const std::vector<uint8_t> &salt, std::vector<uint8_t> &key) {
    key.assign(32, 0);
    return dbackup::crypto::pbkdf2Sha256(password, salt,
                                         dbackup::crypto::kDefaultPbkdf2Iterations, key);
}

bool aesGcm(bool encrypt, const std::vector<uint8_t> &input, const std::vector<uint8_t> &key,
            std::vector<uint8_t> &nonce, std::vector<uint8_t> &tag, std::vector<uint8_t> &output) {
    return dbackup::crypto::aes256Gcm(encrypt, input, key, nonce, tag, output);
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

bool readFile(const std::string &path, std::vector<uint8_t> &data) {
    const std::wstring wide = toWide(path);
    if (wide.empty()) return false;
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(file, &info) != 0 &&
              !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (!ok || size > 256u * 1024u * 1024u || size > static_cast<uint64_t>(data.max_size())) { CloseHandle(file); return false; }
    data.resize(static_cast<size_t>(size));
    size_t offset = 0;
    while (offset < data.size()) {
        const DWORD amount = static_cast<DWORD>(std::min<size_t>(data.size() - offset, MAXDWORD)); DWORD read = 0;
        if (!ReadFile(file, data.data() + offset, amount, &read, nullptr) || read != amount) { ok = false; break; }
        offset += read;
    }
    CloseHandle(file);
    return ok;
}

bool writeAtomic(const std::string &path, const std::vector<uint8_t> &data, bool replace = true) {
    std::vector<uint8_t> random;
    if (!randomBytes(random, 8)) return false;
    const std::wstring target = toWide(path), temporary = toWide(path + ".tmp." + hex(random));
    if (target.empty() || temporary.empty()) return false;
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t offset = 0; bool ok = true;
    while (offset < data.size()) {
        const DWORD amount = static_cast<DWORD>(std::min<size_t>(data.size() - offset, MAXDWORD)); DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, amount, &written, nullptr) || written != amount) { ok = false; break; }
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (ok) ok = MoveFileExW(temporary.c_str(), target.c_str(), flags) != 0;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

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

bool validateStoredChunk(const std::string &root, const std::string &id, const std::vector<uint8_t> &key, std::string &error) {
    if (!repository_path_safety::isHex(id, 64)) { error = "数据块标识无效: " + id; return false; }
    const std::string path = root + "\\chunks\\" + id.substr(0, 2) + "\\" + id + ".blk";
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(toWide(path).c_str(), GetFileExInfoStandard, &attributes)) { error = "数据块缺失: " + id; return false; }
    const uint64_t storedSize = (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    if (attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || storedSize < 32 || storedSize > 64u * 1024u * 1024u + 32u) { error = "数据块大小或类型异常: " + id; return false; }
    std::vector<uint8_t> data;
    if (!readFile(path, data) || data.size() != storedSize || memcmp(data.data(), "DBC1", 4)) { error = "数据块格式无效: " + id; return false; }
    std::vector<uint8_t> nonce(data.begin() + 4, data.begin() + 16), tag(data.begin() + 16, data.begin() + 32), cipher(data.begin() + 32, data.end()), plain;
    if (!aesGcm(false, cipher, key, nonce, tag, plain)) { error = "数据块认证失败: " + id; return false; }
    std::vector<uint8_t> digest;
    if (plain.empty() || !sha256(plain.data(), plain.size(), digest) || hex(digest) != id) { error = "数据块哈希不匹配: " + id; return false; }
    return true;
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
bool loadManifest(const std::string&root,const std::string&id,const std::vector<uint8_t>&key,Manifest&m){if(!repository_path_safety::validSnapshotId(id))return false;std::vector<uint8_t>d;if(!readFile(root+"\\snapshots\\"+id+".manifest",d)||d.size()<32||memcmp(d.data(),"DBE1",4))return false;std::vector<uint8_t>nonce(d.begin()+4,d.begin()+16),tag(d.begin()+16,d.begin()+32),cipher(d.begin()+32,d.end()),plain;if(!aesGcm(false,cipher,key,nonce,tag,plain))return false;m=Manifest{};return deserialize(plain,m)&&m.info.id==id;}
}

LocalRepository::LocalRepository(std::string root):root_(std::move(root)){}

bool LocalRepository::initialize(const std::string &password, std::string &error) {

    if (password.empty() || root_.empty()) { error = "增量仓库路径和密码不能为空"; return false; }
    const std::wstring absoluteRoot = repository_path_safety::absolutePath(toWide(root_));
    if (absoluteRoot.empty() || !repository_path_safety::ensureSafeDirectories(absoluteRoot) ||
        !repository_path_safety::ensureSafeDirectories(absoluteRoot + L"\\chunks") ||
        !repository_path_safety::ensureSafeDirectories(absoluteRoot + L"\\snapshots")) {
        error = "仓库目录无效，或其路径中包含符号链接/目录联接"; return false;
    }
    const std::string configPath = root_ + "\\config.bin";
    const DWORD configAttributes = GetFileAttributesW(toWide(configPath).c_str());
    const DWORD configError = configAttributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    if (configAttributes != INVALID_FILE_ATTRIBUTES) {
        if ((configAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !loadConfig(root_, salt_)) {
            error = "仓库配置损坏，已保留现有数据"; return false;
        }
        std::vector<uint8_t> key;
        if (!deriveKey(password, salt_, key)) { error = "密钥派生失败"; return false; }
        WIN32_FIND_DATAW file; HANDLE find = FindFirstFileW((toWide(root_) + L"\\snapshots\\*.manifest").c_str(), &file);
        bool foundSnapshot = false, allSnapshotsReadable = true;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) { allSnapshotsReadable = false; continue; }
                foundSnapshot = true;
                const std::string name = toUtf8(file.cFileName);
                if (name.size() <= 9 || name.substr(name.size() - 9) != ".manifest") { allSnapshotsReadable = false; continue; }
                const std::string id = name.substr(0, name.size() - 9);
                Manifest manifest;
                if (!repository_path_safety::validSnapshotId(id) || !loadManifest(root_, id, key, manifest)) allSnapshotsReadable = false;
            } while (FindNextFileW(find, &file));
            const DWORD enumerationError = GetLastError(); FindClose(find);
            if (enumerationError != ERROR_NO_MORE_FILES) { error = "读取快照清单时发生错误"; return false; }
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
            error = "无法检查现有快照"; return false;
        }
        if (foundSnapshot && !allSnapshotsReadable) { error = "仓库密码错误或至少一个快照清单已损坏"; return false; }
        activePassword_ = password;
        return true;
    }
    if (configError != ERROR_FILE_NOT_FOUND && configError != ERROR_PATH_NOT_FOUND) { error = "无法检查仓库配置"; return false; }
    WIN32_FIND_DATAW existing; HANDLE find = FindFirstFileW((toWide(root_) + L"\\snapshots\\*.manifest").c_str(), &existing);
    if (find != INVALID_HANDLE_VALUE) { FindClose(find); error = "仓库缺少配置文件，已保留现有快照"; return false; }
    const DWORD snapshotError = GetLastError();
    if (snapshotError != ERROR_FILE_NOT_FOUND && snapshotError != ERROR_PATH_NOT_FOUND) { error = "无法检查仓库快照"; return false; }
    std::vector<uint8_t> salt;
    if (!randomBytes(salt, 16)) { error = "无法生成仓库随机盐"; return false; }
    std::vector<uint8_t> data(kConfigMagic, kConfigMagic + 4); data.insert(data.end(), salt.begin(), salt.end());
    if (!writeAtomic(configPath, data, false)) { error = "无法安全创建仓库配置（配置可能已由其他进程创建）"; return false; }
    salt_ = std::move(salt); activePassword_ = password;
    return true;
}
bool LocalRepository::createSnapshot(const SnapshotOptions &options, SnapshotInfo &snapshot, std::string &error, const OperationContext &context) {
    if (options.sources.empty() || options.password.empty()) { error = "增量仓库需要来源和密码"; return false; }
    if (options.chunkSize == 0 || options.chunkSize > 64u * 1024u * 1024u) { error = "数据块大小必须在 1 字节到 64 MiB 之间"; return false; }
    const std::wstring repositoryPath = repository_path_safety::absolutePath(toWide(root_));
    if (repositoryPath.empty()) { error = "仓库路径无效"; return false; }
    for (const auto &source : options.sources) {
        const std::wstring sourcePath = repository_path_safety::absolutePath(toWide(source));
        if (sourcePath.empty()) { error = "来源路径无效: " + source; return false; }
        if (repository_path_safety::isWithinOrEqual(sourcePath, repositoryPath) || repository_path_safety::isWithinOrEqual(repositoryPath, sourcePath)) {
            error = "来源目录与增量仓库目录不能相互包含: " + source; return false;
        }
    }
    if (!initialize(options.password, error)) return false;
    std::vector<uint8_t> key;
    if (!deriveKey(options.password, salt_, key)) { error = "密钥派生失败"; return false; }
    Manifest manifest;
    manifest.info.createdAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<uint8_t> random;
    if (!randomBytes(random, 8)) { error = "无法生成快照 ID"; return false; }
    manifest.info.id = std::to_string(manifest.info.createdAtMs) + "-" + hex(random);
    std::set<std::string> newChunks;
    for (const auto &source : options.sources) {
        const DWORD attributes = GetFileAttributesW(toWide(source).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) { error = "来源不存在: " + source; return false; }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (!scanSelectedFile(source, options, manifest, root_, key, newChunks, error, context)) return false;
            continue;
        }
        const std::string rootName = baseName(source);
        if (!rootName.empty()) {
            WIN32_FIND_DATAW rootData; HANDLE find = FindFirstFileW(toWide(source).c_str(), &rootData);
            if (find == INVALID_HANDLE_VALUE) { error = "无法读取来源目录: " + source; return false; }
            FindClose(find);
            if (matches(rootData, rootName, options.filter)) {
                ManifestEntry rootEntry; rootEntry.type = 1; rootEntry.path = rootName;
                rootEntry.attributes = rootData.dwFileAttributes; rootEntry.mtimeMs = fileTimeMs(rootData.ftLastWriteTime);
                rootEntry.security = securityDescriptor(toWide(source)); manifest.entries.push_back(std::move(rootEntry));
            }
        }
        if (!scanDirectory(source, rootName, options, manifest, root_, key, newChunks, error, context)) return false;
    }
    for (const auto &entry : manifest.entries) {
        if (entry.type == 2 && entry.linkTarget.empty()) { error = "无法读取重解析点目标: " + entry.path; return false; }
        if (entry.type != 0) continue;
        uint64_t observedSize = 0;
        for (const auto &chunk : entry.chunks) {
            const std::string chunkPath = root_ + "\\chunks\\" + chunk.substr(0, 2) + "\\" + chunk + ".blk";
            WIN32_FILE_ATTRIBUTE_DATA attributes;
            if (!GetFileAttributesExW(toWide(chunkPath).c_str(), GetFileExInfoStandard, &attributes)) { error = "扫描后数据块缺失: " + chunk; return false; }
            const uint64_t storedSize = (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
            if (storedSize < 32 || storedSize > 64u * 1024u * 1024u + 32u || observedSize > UINT64_MAX - (storedSize - 32)) { error = "扫描后数据块大小异常: " + chunk; return false; }
            observedSize += storedSize - 32;
            if (!newChunks.count(chunk) && !validateStoredChunk(root_, chunk, key, error)) return false;
        }
        if (observedSize != entry.size) { error = "备份期间文件发生变化，请重试: " + entry.path; return false; }
    }
    if (context.isCancelled()) { error = "操作已取消"; return false; }
    std::set<std::wstring> uniquePaths;
    for (const auto &entry : manifest.entries) {
        if (!repository_path_safety::safeRelativePath(entry.path)) { error = "来源产生了不安全或空的相对路径"; return false; }
        const std::wstring name = repository_path_safety::utf8ToWide(entry.path);
        std::wstring folded(name.size(), L'\0');
        const int foldedLength = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, name.data(), static_cast<int>(name.size()), folded.data(), static_cast<int>(folded.size()), nullptr, nullptr, 0);
        if (foldedLength <= 0 || !uniquePaths.insert(folded).second) { error = "所选来源存在重名路径，无法无损恢复: " + entry.path; return false; }
    }
    std::vector<uint8_t> plain; serialize(manifest, plain);
    std::vector<uint8_t> nonce, tag, cipher;
    if (!aesGcm(true, plain, key, nonce, tag, cipher)) { error = "清单加密失败"; return false; }
    std::vector<uint8_t> stored = {'D','B','E','1'}; stored.insert(stored.end(), nonce.begin(), nonce.end()); stored.insert(stored.end(), tag.begin(), tag.end()); stored.insert(stored.end(), cipher.begin(), cipher.end());
    if (!writeAtomic(root_ + "\\snapshots\\" + manifest.info.id + ".manifest", stored)) { error = "快照清单提交失败"; return false; }
    snapshot = manifest.info;
    OperationProgress done; done.stage = OperationStage::Completed; done.completedFiles = snapshot.fileCount; done.completedBytes = snapshot.logicalBytes; context.report(done);
    return true;
}

bool LocalRepository::listSnapshots(std::vector<SnapshotInfo> &snapshots, std::string &error) {
    snapshots.clear();
    if (salt_.empty() && !loadConfig(root_, salt_)) { error = "仓库未初始化或配置损坏"; return false; }
    WIN32_FIND_DATAW data; HANDLE find = FindFirstFileW((toWide(root_) + L"\\snapshots\\*.manifest").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return true;
        error = "无法读取快照列表"; return false;
    }
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string filename = toUtf8(data.cFileName);
        if (filename.size() <= 9 || filename.substr(filename.size() - 9) != ".manifest") continue;
        SnapshotInfo snapshot; snapshot.id = filename.substr(0, filename.size() - 9);
        if (!repository_path_safety::validSnapshotId(snapshot.id)) continue;
        try { snapshot.createdAtMs = std::stoll(snapshot.id.substr(0, 13)); } catch (...) { continue; }
        snapshots.push_back(std::move(snapshot));
    } while (FindNextFileW(find, &data));
    const DWORD lastError = GetLastError(); FindClose(find);
    if (lastError != ERROR_NO_MORE_FILES) { error = "读取快照列表时发生错误"; return false; }
    std::sort(snapshots.begin(), snapshots.end(), [](const auto &a, const auto &b) { return a.createdAtMs > b.createdAtMs; });
    return true;
}

bool LocalRepository::restoreSnapshot(const std::string &id, const std::string &destination,
                                      const std::string &password, std::string &error,
                                      const OperationContext &context) {
    error.clear();
    if (!repository_path_safety::validSnapshotId(id)) { error = "快照标识无效"; return false; }
    if (password.empty()) { error = "仓库密码不能为空"; return false; }
    if (salt_.empty() && !loadConfig(root_, salt_)) { error = "仓库配置无效"; return false; }
    std::vector<uint8_t> key;
    if (!deriveKey(password, salt_, key)) { error = "密钥派生失败"; return false; }
    Manifest manifest;
    if (!loadManifest(root_, id, key, manifest)) { error = "密码错误或快照清单损坏"; return false; }
    if (context.isCancelled()) { error = "操作已取消"; return false; }

    const std::wstring destinationWide = repository_path_safety::utf8ToWide(destination);
    const std::wstring destinationAbsolute = repository_path_safety::absolutePath(destinationWide);
    if (destinationAbsolute.empty() || !repository_path_safety::ensureSafeDirectories(destinationAbsolute)) {
        error = "恢复目录无效，或其路径中包含符号链接/目录联接"; return false;
    }

    std::map<std::wstring, uint8_t> paths;
    for (const auto &entry : manifest.entries) {
        if (!repository_path_safety::safeRelativePath(entry.path) || entry.type > 2) {
            error = "快照包含不安全路径或未知条目类型: " + entry.path; return false;
        }
        if (entry.type == 2 && !repository_path_safety::safeRelativePath(entry.linkTarget)) {
            error = "拒绝恢复不安全的链接目标: " + entry.linkTarget; return false;
        }
        if (entry.type != 0 && !entry.chunks.empty()) { error = "非文件条目包含数据块"; return false; }
        if (entry.type == 0 && !entry.linkTarget.empty()) { error = "文件条目包含异常链接目标"; return false; }
        if (!entry.security.empty() && !IsValidSecurityDescriptor(const_cast<uint8_t *>(entry.security.data()))) {
            error = "快照包含无效的安全描述符: " + entry.path; return false;
        }
        for (const auto &chunk : entry.chunks) if (!repository_path_safety::isHex(chunk, 64)) {
            error = "快照包含无效的数据块标识: " + entry.path; return false;
        }
        std::wstring name = repository_path_safety::utf8ToWide(entry.path), folded(name.size(), L'\0');
        const int foldedLength = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, name.data(), static_cast<int>(name.size()), folded.data(), static_cast<int>(folded.size()), nullptr, nullptr, 0);
        if (foldedLength <= 0) { error = "快照路径编码无效"; return false; }
        folded.resize(static_cast<size_t>(foldedLength));
        if (!paths.emplace(folded, entry.type).second) { error = "快照包含重复路径: " + entry.path; return false; }
    }
    for (const auto &entry : manifest.entries) {
        std::wstring name = repository_path_safety::utf8ToWide(entry.path);
        std::replace(name.begin(), name.end(), L'/', L'\\');
        size_t slash = name.find(L'\\');
        while (slash != std::wstring::npos) {
            std::wstring parent = name.substr(0, slash), folded(parent.size(), L'\0');
            const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, parent.data(), static_cast<int>(parent.size()), folded.data(), static_cast<int>(folded.size()), nullptr, nullptr, 0);
            if (count <= 0) { error = "快照路径编码无效"; return false; }
            folded.resize(static_cast<size_t>(count));
            auto found = paths.find(folded);
            if (found != paths.end() && found->second != 1) { error = "快照文件与子路径冲突: " + entry.path; return false; }
            slash = name.find(L'\\', slash + 1);
        }
    }

    // Validate every chunk and file length before creating any restored content.
    for (const auto &entry : manifest.entries) {
        if (entry.type != 0) continue;
        uint64_t total = 0;
        for (const auto &chunk : entry.chunks) {
            if (context.isCancelled()) { error = "操作已取消"; return false; }
            const std::string chunkPath = root_ + "\\chunks\\" + chunk.substr(0, 2) + "\\" + chunk + ".blk";
            WIN32_FILE_ATTRIBUTE_DATA attributes;
            if (!GetFileAttributesExW(toWide(chunkPath).c_str(), GetFileExInfoStandard, &attributes)) { error = "数据块缺失: " + chunk; return false; }
            const uint64_t storedSize = (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
            if (storedSize < 32 || storedSize > 64u * 1024u * 1024u + 32u) { error = "数据块大小异常: " + chunk; return false; }
            std::vector<uint8_t> data;
            if (!readFile(chunkPath, data) || data.size() != storedSize || memcmp(data.data(), "DBC1", 4)) { error = "数据块缺失或损坏: " + chunk; return false; }
            std::vector<uint8_t> nonce(data.begin() + 4, data.begin() + 16), tag(data.begin() + 16, data.begin() + 32), cipher(data.begin() + 32, data.end()), plain;
            if (!aesGcm(false, cipher, key, nonce, tag, plain)) { error = "数据块认证失败: " + chunk; return false; }
            std::vector<uint8_t> digest;
            if (!sha256(plain.data(), plain.size(), digest) || hex(digest) != chunk) { error = "数据块哈希不匹配: " + chunk; return false; }
            if (plain.empty() || total > UINT64_MAX - plain.size()) { error = "数据块长度溢出: " + entry.path; return false; }
            total += plain.size();
        }
        if (total != entry.size) { error = "数据块总长度与文件长度不符: " + entry.path; return false; }
    }

    uint64_t completedFiles = 0;
    const uint64_t totalFiles = std::count_if(manifest.entries.begin(), manifest.entries.end(), [](const ManifestEntry &entry) { return entry.type == 0; });
    auto fullPathFor = [&](const std::string &relative) {
        std::wstring suffix = repository_path_safety::utf8ToWide(relative);
        std::replace(suffix.begin(), suffix.end(), L'/', L'\\');
        return destinationAbsolute + L"\\" + suffix;
    };
    auto applyFileMetadata = [&](const ManifestEntry &entry, const std::wstring &path) {
        HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, (entry.type == 1 ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
        if (handle == INVALID_HANDLE_VALUE) { error = "无法打开项目以恢复时间: " + entry.path; return false; }
        FILETIME time = toFileTime(entry.mtimeMs);
        const bool timeOk = SetFileTime(handle, nullptr, nullptr, &time) != 0;
        CloseHandle(handle);
        if (!timeOk) { error = "无法恢复项目时间: " + entry.path; return false; }
        if (entry.attributes) {
            DWORD attributes = entry.attributes & ~FILE_ATTRIBUTE_REPARSE_POINT;
            if (entry.type == 0) attributes &= ~FILE_ATTRIBUTE_DIRECTORY;
            if (entry.type == 1) attributes |= FILE_ATTRIBUTE_DIRECTORY;
            if (attributes == 0) attributes = FILE_ATTRIBUTE_NORMAL;
            if (!SetFileAttributesW(path.c_str(), attributes)) { error = "无法恢复项目属性: " + entry.path; return false; }
        }
        if (!entry.security.empty()) {
            auto *descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(const_cast<uint8_t *>(entry.security.data()));
            if (!SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, descriptor)) {
                if (error.empty()) error = "数据已恢复，但当前账户无权恢复访问控制列表: " + entry.path;
            }
            if (!SetFileSecurityW(path.c_str(), OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION, descriptor)) {
                error = "数据已恢复，但当前账户无权恢复属主/组: " + entry.path;
            }
        }
        return true;
    };

    for (const auto &entry : manifest.entries) {
        if (context.isCancelled()) { error = "操作已取消"; return false; }
        const std::wstring path = fullPathFor(entry.path);
        if (entry.type == 1) {
            if (!repository_path_safety::ensureSafeDirectories(path)) { error = "无法安全创建目录: " + entry.path; return false; }
            continue;
        }
        const size_t slash = path.find_last_of(L'\\');
        const std::wstring parent = slash == std::wstring::npos ? destinationAbsolute : path.substr(0, slash);
        if (!repository_path_safety::ensureSafeDirectories(parent)) { error = "恢复路径的父目录无效或包含链接: " + entry.path; return false; }
        const DWORD existing = GetFileAttributesW(path.c_str());
        if (existing != INVALID_FILE_ATTRIBUTES && ((existing & FILE_ATTRIBUTE_REPARSE_POINT) || (entry.type == 0 && (existing & FILE_ATTRIBUTE_DIRECTORY)))) {
            error = "恢复目标已存在为链接或目录，拒绝覆盖: " + entry.path; return false;
        }
        if (entry.type == 2) {
            DWORD flags = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
            flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
            if (!CreateSymbolicLinkW(path.c_str(), repository_path_safety::utf8ToWide(entry.linkTarget).c_str(), flags)) {
                error = "无法恢复符号链接: " + entry.path; return false;
            }
            continue;
        }
        std::vector<uint8_t> random;
        if (!randomBytes(random, 8)) { error = "无法生成恢复临时文件名"; return false; }
        const std::string randomName = hex(random);
        const std::wstring temporary = path + L".tmp.restore." + std::wstring(randomName.begin(), randomName.end());
        HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) { error = "无法创建恢复临时文件: " + entry.path; return false; }
        bool writeOk = true;
        for (const auto &chunk : entry.chunks) {
            if (context.isCancelled()) { error = "操作已取消"; writeOk = false; break; }
            std::vector<uint8_t> data;
            if (!readFile(root_ + "\\chunks\\" + chunk.substr(0, 2) + "\\" + chunk + ".blk", data) || data.size() < 32 || memcmp(data.data(), "DBC1", 4)) { error = "数据块缺失或损坏: " + chunk; writeOk = false; break; }
            std::vector<uint8_t> nonce(data.begin() + 4, data.begin() + 16), tag(data.begin() + 16, data.begin() + 32), cipher(data.begin() + 32, data.end()), plain;
            if (!aesGcm(false, cipher, key, nonce, tag, plain)) { error = "数据块认证失败: " + chunk; writeOk = false; break; }
            std::vector<uint8_t> digest;
            if (!sha256(plain.data(), plain.size(), digest) || hex(digest) != chunk) { error = "数据块哈希不匹配: " + chunk; writeOk = false; break; }
            size_t offset = 0;
            while (offset < plain.size()) {
                const DWORD amount = static_cast<DWORD>(std::min<size_t>(plain.size() - offset, MAXDWORD)); DWORD written = 0;
                if (!WriteFile(output, plain.data() + offset, amount, &written, nullptr) || written != amount) { error = "恢复文件写入失败: " + entry.path; writeOk = false; break; }
                offset += written;
            }
            if (!writeOk) break;
        }
        if (writeOk && !FlushFileBuffers(output)) { error = "恢复文件刷新失败: " + entry.path; writeOk = false; }
        CloseHandle(output);
        if (!writeOk) { DeleteFileW(temporary.c_str()); return false; }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str()); error = "无法提交恢复文件: " + entry.path; return false;
        }
        if (!applyFileMetadata(entry, path)) return false;
        OperationProgress progress; progress.stage = OperationStage::Restoring; progress.currentPath = entry.path;
        progress.completedFiles = ++completedFiles; progress.totalFiles = totalFiles;
        context.report(progress);
    }
    for (auto iterator = manifest.entries.rbegin(); iterator != manifest.entries.rend(); ++iterator) {
        if (iterator->type != 1) continue;
        if (context.isCancelled()) { error = "操作已取消"; return false; }
        if (!applyFileMetadata(*iterator, fullPathFor(iterator->path))) return false;
    }
    OperationProgress done; done.stage = OperationStage::Completed; done.completedFiles = totalFiles; done.totalFiles = totalFiles; context.report(done);
    return true;
}
bool LocalRepository::deleteSnapshot(const std::string &id, std::string &error) {
    if (!repository_path_safety::validSnapshotId(id)) { error = "快照标识无效"; return false; }
    const std::wstring path = toWide(root_ + "\\snapshots\\" + id + ".manifest");
    if (path.empty() || !DeleteFileW(path.c_str())) { error = "快照不存在或无法删除"; return false; }
    return true;
}

bool LocalRepository::prune(size_t keep, std::string &error) {
    std::vector<SnapshotInfo> all;
    if (!listSnapshots(all, error)) return false;
    std::set<std::string> liveChunks;
    if (keep > 0 && !activePassword_.empty()) {
        std::vector<uint8_t> key;
        if (!deriveKey(activePassword_, salt_, key)) { error = "无法验证保留快照"; return false; }
        for (size_t i = 0; i < std::min(keep, all.size()); ++i) {
            Manifest manifest;
            if (!loadManifest(root_, all[i].id, key, manifest)) { error = "保留快照清单损坏，已取消清理"; return false; }
            for (const auto &entry : manifest.entries) for (const auto &chunk : entry.chunks) {
                if (!repository_path_safety::isHex(chunk, 64)) { error = "保留快照包含无效数据块引用"; return false; }
                const std::string chunkPath = root_ + "\\chunks\\" + chunk.substr(0, 2) + "\\" + chunk + ".blk";
                if (GetFileAttributesW(toWide(chunkPath).c_str()) == INVALID_FILE_ATTRIBUTES) { error = "保留快照的数据块缺失，已取消清理"; return false; }
                liveChunks.insert(chunk);
            }
        }
    }
    for (size_t i = keep; i < all.size(); ++i) if (!deleteSnapshot(all[i].id, error)) return false;
    if (activePassword_.empty()) return true;
    WIN32_FIND_DATAW directory; HANDLE directories = FindFirstFileW((toWide(root_) + L"\\chunks\\*").c_str(), &directory);
    if (directories == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (!(directory.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || directory.cFileName[0] == L'.' ||
            (directory.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::string folder = toUtf8(directory.cFileName);
        WIN32_FIND_DATAW file; HANDLE files = FindFirstFileW((toWide(root_ + "\\chunks\\" + folder) + L"\\*.blk").c_str(), &file);
        if (files == INVALID_HANDLE_VALUE) continue;
        do {
            if (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string name = toUtf8(file.cFileName);
            if (name.size() != 68 || name.substr(64) != ".blk") continue;
            const std::string id = name.substr(0, 64);
            if (!repository_path_safety::isHex(id, 64) || liveChunks.count(id)) continue;
            if (!DeleteFileW(toWide(root_ + "\\chunks\\" + folder + "\\" + name).c_str())) {
                FindClose(files); FindClose(directories); error = "无法清理未引用的数据块"; return false;
            }
        } while (FindNextFileW(files, &file));
        FindClose(files);
    } while (FindNextFileW(directories, &directory));
    const DWORD lastError = GetLastError(); FindClose(directories);
    if (lastError != ERROR_NO_MORE_FILES) { error = "清理数据块时发生错误"; return false; }
    return true;
}
