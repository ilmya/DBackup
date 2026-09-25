#include "security/credential_store.h"
#include <windows.h>
#include <wincred.h>
#include <vector>

namespace { std::wstring wide(const std::string&s){int n=MultiByteToWideChar(CP_UTF8,0,s.data(),s.size(),nullptr,0);std::wstring w(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),s.size(),w.data(),n);return w;} }
bool CredentialStore::save(const std::string&name,const std::string&secret,std::string&error){std::wstring target=wide("DBackup/"+name);CREDENTIALW value={};value.Type=CRED_TYPE_GENERIC;value.TargetName=const_cast<wchar_t*>(target.c_str());value.CredentialBlobSize=static_cast<DWORD>(secret.size());value.CredentialBlob=reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));value.Persist=CRED_PERSIST_LOCAL_MACHINE;value.UserName=const_cast<wchar_t*>(L"DBackup");if(!CredWriteW(&value,0)){error="无法写入 Windows 凭据管理器";return false;}return true;}
bool CredentialStore::load(const std::string&name,std::string&secret,std::string&error){std::wstring target=wide("DBackup/"+name);PCREDENTIALW value=nullptr;if(!CredReadW(target.c_str(),CRED_TYPE_GENERIC,0,&value)){error="凭据不存在";return false;}secret.assign(reinterpret_cast<char*>(value->CredentialBlob),value->CredentialBlobSize);CredFree(value);return true;}
bool CredentialStore::remove(const std::string&name,std::string&error){std::wstring target=wide("DBackup/"+name);if(!CredDeleteW(target.c_str(),CRED_TYPE_GENERIC,0)&&GetLastError()!=ERROR_NOT_FOUND){error="无法删除 Windows 凭据";return false;}return true;}
