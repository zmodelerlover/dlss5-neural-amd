#pragma once
#include <windows.h>
#include "bridge_ipc.h"
namespace x86bridge {
inline constexpr DWORD IpcTimeoutMs=5000;
inline constexpr DWORD StartupTimeoutMs=60000;
struct Handle {
    HANDLE value=nullptr;
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    ~Handle(){reset();}
    void reset(HANDLE h=nullptr){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=h;}
    explicit operator bool()const{return value&&value!=INVALID_HANDLE_VALUE;}
};
// CPU barrier only. Wait for this I/O, a bounded timeout, or peer process death.
// Cancelled I/O is retired before the stack OVERLAPPED/buffer can disappear.
inline bool Transfer(HANDLE pipe,HANDLE peer,void* data,uint32_t size,bool write,DWORD timeoutMs=IpcTimeoutMs){
    auto* at=static_cast<unsigned char*>(data);
    while(size){
        Handle ev(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!ev)return false;
        OVERLAPPED ov{};ov.hEvent=ev.value;DWORD n=0;
        BOOL ok=write?WriteFile(pipe,at,size,&n,&ov):ReadFile(pipe,at,size,&n,&ov);
        if(!ok){
            const DWORD pendingError=GetLastError();
            if(pendingError!=ERROR_IO_PENDING){ev.reset();SetLastError(pendingError);return false;}
            HANDLE waits[]={ev.value,peer};const DWORD rc=WaitForMultipleObjects(2,waits,FALSE,timeoutMs);
            if(rc!=WAIT_OBJECT_0){
                const DWORD error=rc==WAIT_TIMEOUT?ERROR_TIMEOUT:
                    rc==WAIT_OBJECT_0+1?ERROR_BROKEN_PIPE:GetLastError();
                CancelIoEx(pipe,&ov);GetOverlappedResult(pipe,&ov,&n,TRUE);
                ev.reset();SetLastError(error);return false;
            }
            if(!GetOverlappedResult(pipe,&ov,&n,FALSE)){
                const DWORD error=GetLastError();ev.reset();SetLastError(error);return false;
            }
        }
        if(!n||n>size){ev.reset();SetLastError(ERROR_INVALID_DATA);return false;}at+=n;size-=n;
    }
    return true;
}
inline bool Send(HANDLE p,HANDLE peer,const void* b,uint32_t n,DWORD timeoutMs=IpcTimeoutMs){return Transfer(p,peer,const_cast<void*>(b),n,true,timeoutMs);}
inline bool Receive(HANDLE p,HANDLE peer,void* b,uint32_t n,DWORD timeoutMs=IpcTimeoutMs){return Transfer(p,peer,b,n,false,timeoutMs);}
// A request is a post followed by the collection of its answer. The two halves are separable
// because the pipelined present path posts a frame, lets the game run, and collects the answer in
// the next present. Request keeps the two joined for every other caller, so the wire rules -- the
// header validation on the way out and the magic/version/kind/size checks on the way back -- are
// written once and cannot drift between the two paths.
//
// The pipe carries one conversation. A posted request whose answer has not been collected will be
// answered into whatever call reads next, so a caller that posts owns the obligation to collect
// before issuing anything else on the same pipe.
inline bool Post(HANDLE p,HANDLE peer,Kind k,const void* body,uint32_t bytes,DWORD timeoutMs=IpcTimeoutMs){
    Header h;h.kind=k;h.bytes=bytes;
    return ValidHeader(h)&&Send(p,peer,&h,sizeof(h),timeoutMs)&&(!bytes||Send(p,peer,body,bytes,timeoutMs));
}
inline bool Collect(HANDLE p,HANDLE peer,Kind k,Ack& ack,DWORD timeoutMs=IpcTimeoutMs){
    return Receive(p,peer,&ack,sizeof(ack),timeoutMs)&&ack.header.magic==Magic&&ack.header.version==Version&&ack.header.kind==k&&ack.header.bytes==sizeof(Ack)-sizeof(Header);
}
inline bool Request(HANDLE p,HANDLE peer,Kind k,const void* body,uint32_t bytes,Ack& ack,DWORD timeoutMs=IpcTimeoutMs){
    return Post(p,peer,k,body,bytes,timeoutMs)&&Collect(p,peer,k,ack,timeoutMs);
}
}
