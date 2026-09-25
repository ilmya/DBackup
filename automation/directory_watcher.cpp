#include "automation/directory_watcher.h"
#include <windows.h>
#include <chrono>
#include <set>

namespace { std::wstring wide(const std::string&s){int n=MultiByteToWideChar(CP_UTF8,0,s.data(),s.size(),nullptr,0);std::wstring w(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),s.size(),w.data(),n);return w;}std::string utf8(const std::wstring&w){int n=WideCharToMultiByte(CP_UTF8,0,w.data(),w.size(),nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,w.data(),w.size(),s.data(),n,nullptr,nullptr);return s;} }
DirectoryWatcher::DirectoryWatcher()=default;
DirectoryWatcher::~DirectoryWatcher(){stop();}
bool DirectoryWatcher::start(const std::string&path,Callback callback,std::string&error,unsigned debounce,unsigned maximum){stop();HANDLE h=CreateFileW(wide(path).c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);if(h==INVALID_HANDLE_VALUE){error="无法监控目录: "+path;return false;}path_=path;callback_=std::move(callback);debounceMs_=debounce;maximumDelayMs_=maximum;directoryHandle_=h;running_=true;worker_=std::thread(&DirectoryWatcher::run,this);return true;}
void DirectoryWatcher::stop(){if(!running_.exchange(false))return;CancelIoEx(static_cast<HANDLE>(directoryHandle_),nullptr);if(worker_.joinable())worker_.join();CloseHandle(static_cast<HANDLE>(directoryHandle_));directoryHandle_=nullptr;}
void DirectoryWatcher::run(){std::vector<uint8_t>buffer(64*1024);std::set<std::string>changes;auto first=std::chrono::steady_clock::now(),last=first;bool active=false,overflow=false;while(running_){DWORD bytes=0;BOOL ok=ReadDirectoryChangesW(static_cast<HANDLE>(directoryHandle_),buffer.data(),buffer.size(),TRUE,FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_CREATION,&bytes,nullptr,nullptr);if(!running_)break;auto now=std::chrono::steady_clock::now();if(!ok||bytes==0){overflow=true;}else{size_t offset=0;while(offset<bytes){auto*info=reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data()+offset);changes.insert(utf8(std::wstring(info->FileName,info->FileNameLength/sizeof(wchar_t))));if(info->NextEntryOffset==0)break;offset+=info->NextEntryOffset;}}if(!active){first=now;active=true;}last=now;
        // ReadDirectoryChangesW is synchronous; emit immediately after a burst returned. The caller
        // performs the final debounce before launching a snapshot.
        if(active&&(overflow||std::chrono::duration_cast<std::chrono::milliseconds>(now-first).count()>=maximumDelayMs_||std::chrono::duration_cast<std::chrono::milliseconds>(now-last).count()>=debounceMs_)){callback_(std::vector<std::string>(changes.begin(),changes.end()),overflow);changes.clear();overflow=false;active=false;}else if(active){callback_(std::vector<std::string>(changes.begin(),changes.end()),overflow);changes.clear();overflow=false;active=false;}}
}
