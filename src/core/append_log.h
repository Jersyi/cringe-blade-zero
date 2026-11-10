#pragma once
#include <windows.h>
#include <string>
#include <cstdint>

// Append/rotate serially across UI and game threads. Keep the handle between
// messages; no per-line open/stat/close on animation and model-switch paths.
class AppendLog {
    SRWLOCK lock_=SRWLOCK_INIT;HANDLE file_=INVALID_HANDLE_VALUE;
    std::wstring path_;uint64_t bytes_=0,limit_;
    void CloseUnlocked(){if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);file_=INVALID_HANDLE_VALUE;bytes_=0;}
    bool Open(){
        file_=CreateFileW(path_.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file_==INVALID_HANDLE_VALUE)return false;
        LARGE_INTEGER size{};if(!GetFileSizeEx(file_,&size)){CloseUnlocked();return false;}bytes_=static_cast<uint64_t>(size.QuadPart);return true;
    }
public:
    // Retain one previous segment. Rotation stays synchronous, so make the
    // infrequent rename/open work less frequent without changing write ordering.
    explicit AppendLog(uint64_t limit=16ull*1024*1024):limit_(limit){}
    ~AppendLog(){Close();}
    AppendLog(const AppendLog&)=delete;AppendLog& operator=(const AppendLog&)=delete;
    void Close(){AcquireSRWLockExclusive(&lock_);CloseUnlocked();ReleaseSRWLockExclusive(&lock_);}
    bool Write(const std::wstring& path,const char* text,DWORD length){
        if(path.empty()||!text||!length)return false;
        AcquireSRWLockExclusive(&lock_);
        struct Unlock{SRWLOCK* lock;~Unlock(){ReleaseSRWLockExclusive(lock);}} unlock{&lock_};
        if(path_!=path){CloseUnlocked();path_=path;}
        if(file_==INVALID_HANDLE_VALUE&&!Open())return false;
        if(limit_&&bytes_>=limit_){
            CloseUnlocked();if(!MoveFileExW(path_.c_str(),(path_+L".previous").c_str(),MOVEFILE_REPLACE_EXISTING)||!Open())return false;
        }
        DWORD total=0;
        while(total<length){DWORD written=0;if(!WriteFile(file_,text+total,length-total,&written,nullptr)||!written){CloseUnlocked();return false;}total+=written;bytes_+=written;}
        return true;
    }
};
