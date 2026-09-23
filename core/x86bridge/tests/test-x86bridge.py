"""Portable x86 bridge contract and control-flow tests. Not GPU validation."""
from pathlib import Path
import os,re,subprocess,tempfile,shutil
root=Path(__file__).resolve().parents[3]
new=root/'core/x86bridge'
read=lambda path:path.read_text(encoding='utf-8-sig')
f=read(new/'frontend32.cpp');h=read(new/'host64.cpp');ipc=read(new/'bridge_ipc.h');io=read(new/'bridge_io.h')
for word in ['d3d12','amdhip','dlssnr_amd_pass','Packet','RecordFn','ID3D11Device5','ID3D11DeviceContext4','OpenSharedFence','SetEventOnCompletion']:
 assert word.lower() not in f.lower(),word
for word in ['VORT','Generic Depth','async_home','amd_last_nr','g.sent_n','retryAfter','soft_timeout','ping-pong']:
 assert word not in f+h+ipc+io,word
assert '#include "../addon/neural.cpp"' in h and '#define AMDNR_WITH_VULKAN 0' in h
assert h.count('DllMain(')==0
assert 'EnumAdapterByLuid' in h and 'got.LowPart==luid.LowPart&&got.HighPart==luid.HighPart' in h
assert 'DuplicateHandle(GetCurrentProcess(),source.handle.value,g.process.value' in f
assert 'CreateSharedHandle(' in f and 'OpenSharedHandle(handle.value' in h
assert 'GetModuleFileNameW(addonModule' in f and 'addonModule=module;' in f
for marker in ['device_api::d3d9','InitD3D9Bridge','OpenSharedResource(inputHandle',
               'GetRenderTargetData','D3D11_MAP_WRITE','D3D11_MAP_READ','UpdateSurface']:
 assert marker in f,marker
assert 'reinterpret_cast<IDirect3DDevice9 *>(reshadeDevice->get_native())' in f
assert 'dgvoodoo' not in f.lower()
assert 'PIPE_REJECT_REMOTE_CLIENTS' in f and 'GetNamedPipeClientProcessId' in f
assert 'IpcTimeoutMs=5000' in io and 'StartupTimeoutMs=60000' in io
assert 'WaitForMultipleObjects(2,waits,FALSE,timeoutMs)' in io
assert 'ERROR_TIMEOUT' in io and 'CancelIoEx(pipe,&ov)' in io
assert 'x86bridge::StartupTimeoutMs' in f
# All protocol fields have explicitly sized scalar or packed protocol types.
for body in re.findall(r'struct \w+\s*\{(.*?)\};',ipc,re.S):
 assert not re.search(r'\b(?:bool|size_t|HANDLE|uintptr_t|intptr_t|long|double)\b|\*|std::string',body),body
# The normal return never references the shared output before the answer is confirmed. The frame
# that answer belongs to is the one just captured in same-frame mode and the one posted by the
# previous present in pipelined mode, which is why the confirmation names answeredFrame.
present=f[f.index('void OnPresent('):f.index('\n}\nextern "C"')]
assert present.index('CopyResource(g.stageIn11')<present.index('CopyResource(g.colour.on11')<present.index('FlushAndWait11()')<present.index('Kind::Frame,&f')<present.index('Confirmed(a,answeredFrame,g.transport)')<present.index('CopyResource(g.stageOut11')<present.index('CopyResource(bb.Get(),g.stageOut11')
assert present.index('UploadD3D9Frame(bb9.Get())')<present.index('Kind::Frame,&f')<present.index('DownloadD3D9Frame(bb9.Get())')
assert present.count('DeferD3D9Failure(')==2 and present.count('FaultHresult(')==2
recover_start=f.index('bool DeferD3D9Failure(')
recover=f[recover_start:f.index('bool DropRemote(',recover_start)]
assert 'TestCooperativeLevel()' in recover and 'D3DERR_DEVICELOST' in recover and 'D3DERR_DEVICENOTRESET' in recover
assert 'g.reset=true;' in recover and 'StopHost(' not in recover and 'g.failed=true' not in recover
assert 'HRESULT UploadD3D9Frame(' in f and 'HRESULT DownloadD3D9Frame(' in f and 'HRESULT FlushAndWait9()' in f
assert 'mods&g.toggleMods' in present and 'IsIconic' in present and 'g.reset=true' in present
assert 'g.game11ctx->ClearState()' in f
# D3D9 Reset is already in progress when ReShade emits destroy_swapchain. Its resize branch must
# release default-pool resources without IPC, submitting queries or waiting on either GPU. Remote
# retirement is deferred until Bridge::Ensure runs from the next stable presentation.
destroy=f[f.index('void OnDestroy('):f.index('void OnDestroyDevice(')]
d3d9_reset=destroy[destroy.index('if(resize&&g.nativeD3D9)'):destroy.index('\n    }',destroy.index('if(resize&&g.nativeD3D9)'))]
assert 'ReleaseLocal();' in d3d9_reset and 'return;' in d3d9_reset
for unsafe in ['DropRemote(', 'FlushAndWait9(', 'FlushAndWait11(', 'ClearState(', 'Flush(']:
 assert unsafe not in d3d9_reset,unsafe
assert destroy.index('if(resize&&g.nativeD3D9)')<destroy.index('DropRemote();')
# A game that leaves through ExitProcess never delivers destroy_swapchain, and everything this
# add-on holds is released there. Without a second release point ReShade finds its own device still
# referenced -- ComPtr AddRefs the game's device on both routes -- and reports leaked resources.
# destroy_device is the last callback before the device goes, and it is not under the loader lock.
assert 'addon_event::destroy_device>(OnDestroyDevice)' in f
gone=f[f.index('void OnDestroyDevice('):f.index('void OnPresent(')]
assert 'g.game9.Reset()' in gone and 'g.game11.Reset()' in gone and 'ReleaseLocal();' in gone
# It must act only on the device it actually holds, never on another one being torn down.
assert 'native!=ours' in gone and 'g.nativeD3D9?' in gone
# And issue no GPU work: the device is already going away, the same rule the reset branch follows.
for unsafe in ['FlushAndWait','ClearState(','->Flush()','CopyResource','Kind::Quit','x86bridge::Request']:
 assert unsafe not in gone,unsafe

assert 'const std::wstring name=L"\\\\\\\\.\\\\pipe\\\\amd-nr-x86bridge-"' in f
# The stage probe measures; it must never participate. It stays off unless the environment asks
# for it, and it may only read boundaries the frame already crosses -- a wait of its own would
# land inside the D3D9 reset window the frontend keeps clear.
assert 'AMDNR_X86BRIDGE_TIMING' in f
sp=f[f.index('struct StageProbe'):f.index('} probe;')]
assert 'bool on=false;' in sp and 'QueryPerformanceFrequency' in sp and 'QueryPerformanceCounter' in sp
for unsafe in ['FlushAndWait','CreateQuery','Issue(','GetData(','Sleep(','ClearState(','Request(','Flush()','Map(','CopyResource']:
 assert unsafe not in sp,unsafe
# Only a frame that reached the game again is a sample, and it is counted once. Two marks and four
# splits now: the wait for a pipelined answer happens before SyncControls, far from the wait a
# same-frame request makes, and the reported host cost is the sum so both modes stay comparable.
assert present.count('probe.Keep(')==1 and present.count('probe.Begin()')==2 and present.count('probe.Split()')==4
assert present.index('probe.Begin()')<present.index('collectMs=probe.Split()')<present.index('SyncControls()')
assert present.index('inputMs=probe.Split()')<present.index('Kind::Frame,&f')<present.index('requestMs=probe.Split()')
assert present.index('requestMs=probe.Split()')<present.index('Confirmed(a,answeredFrame,g.transport)')<present.index('probe.Keep(')
assert 'probe.Keep(inputMs,collectMs+requestMs,probe.Split())' in present
# Pipelining is the default, and Async=0 has to keep restoring same-frame presentation: the
# guarantee it gives up is deliberate, so the escape hatch is part of the contract.
assert 'L"Async",1,ini.c_str()' in f
# The mode can be switched while the game runs. It is idempotent, it says so in the log, and it
# persists one key rather than rewriting the file, which would drop Timing and the helper's own
# settings. It must not issue IPC or GPU work: the switch costs at most one frame either way
# precisely because nothing has to be reconciled.
setasync=f[f.index('void SetAsync(bool async){'):f.index('void ClearGuide(')]
assert 'if(g.async==async)return;' in setasync and 'WritePrivateProfileStringW(L"amd-nr",L"Async"' in setasync
assert 'presentation switched to' in setasync
for unsafe in ['Request(','Post(','Collect(','FlushAndWait','CopyResource','StopHost']:
 assert unsafe not in setasync,unsafe
# A period or stage window that spans a mode switch would average two different things and read as
# one, so both accumulators are thrown away when the mode changes.
assert 'void Present(bool effectOn,bool pipelined)' in sp
assert 'effectOn==periodEffect&&pipelined==periodPipelined' in sp
assert 'if(pipelined!=periodPipelined)Drop();' in sp
assert 'probe.Present(g.enabled,g.async)' in present
# The pipe carries one conversation. A posted frame must be collected before anything else uses the
# pipe, or its answer is delivered into an unrelated call: hence before SyncControls, which talks
# every present, and before OnDestroy touches DropRemote or Quit.
assert present.index('CollectPending(pendingAck,havePending)')<present.index('SyncControls()')
destroy_sc=f[f.index('void OnDestroy('):f.index('void OnDestroyDevice(')]
assert destroy_sc.index('CollectPending(')<destroy_sc.index('DropRemote()')
assert destroy_sc.index('CollectPending(')<destroy_sc.index('ReleaseLocal()')
# One output texture, so the helper may not be given new work until the last result has left it.
assert present.index('Confirmed(a,answeredFrame,g.transport)')<present.index('x86bridge::Post(')
assert present.count('x86bridge::Post(')==1
# Every fault path runs StopHost, so clearing the outstanding frame there is what keeps them safe.
assert 'g.pending=false;' in f[f.index('void StopHost()'):f.index('void Fault(')]
# An answer can outlive what it describes in two ways that Confirmed cannot see, because the answer
# does agree with the frame that asked for it: a resize, and a rebuild in BuildRemote, which runs
# between the post and the compose and replaces the output texture the result was written into.
assert 'g.pendingFrame.generation==g.generation&&g.pendingWidth==width&&g.pendingHeight==height' in present
assert present.index('BuildRemote()')<present.index('g.pendingFrame.generation==g.generation')
# The present-period sample is what the pipelining estimate needs, so it has to keep measuring
# while the effect is off: it sits before the early-outs rather than beside the stage splits, and
# it is read once per present. A window that spans a toggle is thrown away, because averaging the
# effect's frames together with the game's own would answer neither question.
assert present.count('probe.Present(g.enabled,g.async)')==1
assert present.index('probe.Present(g.enabled,g.async)')<present.index('if(g.active&&g.active!=sc)return;')
assert present.index('probe.Present(g.enabled,g.async)')<present.index('probe.Begin()')
assert 'effectOn==periodEffect' in sp and 'periodFrames=0;periodEffect=effectOn;' in sp
assert 'kPeriodOutlierMs' in sp
# The rejected classic-D3D9 raster experiment must not come back with it.
for word in ['FrameFlagClassicD3D9','EfficientClassicD3D9Scale','D3D9 timing avg']:
 assert word not in f,word

# Verify the copied job-pending decision has identical executable text to upstream.
u=read(root/'core/addon/neural.cpp')
# Ends at RenderEffectsAheadOfNetwork rather than at the comment after it: that call is the
# D3D12 route's own business and has no counterpart in the bridge, so including it made this
# compare a policy against a policy plus one unrelated line.
t=read(root/'core/transport/d3d11/D3D11Transport.inc')
a=t[t.index('    bool runNetwork = true;',t.index('void BridgePresent')):t.index('    RenderEffectsAheadOfNetwork',t.index('void BridgePresent'))]
b=h[h.index('    bool runNetwork = true;'):h.index('    const UINT i =',h.index('    bool runNetwork = true;'))]
normal=lambda s:re.sub(r'\s+','',re.sub(r'//[^\n]*','',s))
assert normal(a)==normal(b),'job pending policy differs'
# The guide selection is one header both routes include, so the two cannot pick differently.
for src in (u,f):
 assert 'guide_choice.h"' in src
 for fn in ['DXGI_FORMAT GuideDepthSrvFormat(','bool LooksLikeMotion(','void SettleGuide(','struct Tallied']:
  assert fn not in src,fn
# The guide-depth shader is one header both routes include, so they cannot compile different ones.
for src in (u,f):
 assert '#include "../shaders/guide_depth.h"' in src and 'constexpr char kGuideDepthCs' not in src

transport=h[h.index('    Result CopyOnly()'):h.index('    Result Neural()')]
for call in ['InitHip(','InitEngine(','BringUpEngines(','RecordNetwork(','LoadLibrary','RuntimeHashMatches(']:assert call not in transport
assert 'cmd->CopyResource(g.bridge.crossLocal.Get(),g.bridge.in.on12.Get())' in transport
assert 'cmd->CopyResource(g.bridge.out.on12.Get(),g.bridge.crossLocal.Get())' in transport
assert 'Idle();return Result::Transport' in transport
neural=h[h.index('    Result Neural()'):h.index('    Result FrameWork(')]
assert neural.index('RecordNetwork(')<neural.index('CompositionIsFresh(')<neural.index('cmd->Close()')<neural.index('ExecuteCommandLists(')<neural.index('NotifyFn')<neural.index('WaitForWorkQueue(g.completion)')<neural.index('return fresh?')
assert 'runNetwork&&g.activePasses!=0' in neural and 'g.fence->GetCompletedValue()>=g.completion' in neural
assert h.count('++g.status.frame')==1 and 'Idle();ReleaseSwapchainSized();built=false;g.historyValid.store(false);' in h
for path in [new/'frontend32.cpp',new/'host64.cpp']:
 text=read(path)
 assert 'ProfileForThisProcess' not in text and 'kTargets' not in text
 assert set(re.findall(r'[A-Za-z0-9_-]+\.exe',text)) <= {'amd-nr-host64.exe'}
print('PASS static boundaries: original engine TU; no neural imports/API in frontend; generic LUID match; fixed-width IPC; shared handle ownership; same-frame confirmation; host output fence before ACK; transport isolated; original pending policy; no cached output')
compiler=os.environ.get('CXX') or shutil.which('g++')
if not compiler:raise SystemExit('Set CXX to a C++20 compiler; native MSVC checks are separate')
with tempfile.TemporaryDirectory(prefix='x86bridge-tests-') as d:
 p=Path(d)
 subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror',str(new/'protocol_test.cpp'),'-o',str(p/'protocol')],check=True)
 subprocess.run([str(p/'protocol')],check=True)
 # Test real transfer helper using Win32 doubles, including short reads and peer death.
 (p/'windows.h').write_text(r'''
#pragma once
#include <cstdint>
#include <algorithm>
#include <cassert>
#include <cstring>
using HANDLE=void*;using DWORD=uint32_t;using BOOL=int;
constexpr BOOL FALSE=0,TRUE=1;const HANDLE INVALID_HANDLE_VALUE=reinterpret_cast<HANDLE>(-1);
constexpr DWORD INFINITE=0xffffffff,ERROR_IO_PENDING=997,ERROR_TIMEOUT=1460,ERROR_BROKEN_PIPE=109,ERROR_INVALID_DATA=13,WAIT_OBJECT_0=0,WAIT_TIMEOUT=258;
struct OVERLAPPED{HANDLE hEvent;};
inline int mode=0,calls=0,cancelled=0,retired=0,closed=0,waited=0;inline DWORD amount=0,lastError=ERROR_IO_PENDING;inline void* target=nullptr;
inline BOOL CloseHandle(HANDLE){++closed;return 1;}
inline HANDLE CreateEventW(void*,BOOL,BOOL,void*){return reinterpret_cast<HANDLE>(1);}
inline BOOL ReadFile(HANDLE,void* b,DWORD n,DWORD* done,OVERLAPPED*){++calls;amount=std::min(n,DWORD(3));target=b;if(mode==1||mode==2||mode==5){lastError=ERROR_IO_PENDING;return 0;}*done=mode==3?0:amount;memset(b,42,*done);return 1;}
inline BOOL WriteFile(HANDLE h,void* b,DWORD n,DWORD* done,OVERLAPPED* ov){return ReadFile(h,b,n,done,ov);}
inline DWORD GetLastError(){return mode==4?5:lastError;}inline void SetLastError(DWORD e){lastError=e;}
inline DWORD WaitForMultipleObjects(DWORD n,HANDLE*,BOOL,DWORD timeout){assert(n==2&&timeout==5000);++waited;return mode==2?1:mode==5?WAIT_TIMEOUT:0;}
inline BOOL CancelIoEx(HANDLE,OVERLAPPED*){++cancelled;return 1;}
inline BOOL GetOverlappedResult(HANDLE,OVERLAPPED*,DWORD* n,BOOL wait){if(wait){++retired;return 0;}*n=amount;memset(target,42,amount);return 1;}
''',encoding='utf-8')
 (p/'io.cpp').write_text(r'''
#include "bridge_io.h"
#include <cstdio>
int main(){char b[19]{};HANDLE pipe=reinterpret_cast<HANDLE>(2),peer=reinterpret_cast<HANDLE>(3);
 assert(x86bridge::Receive(pipe,peer,b,sizeof(b)));assert(calls==7&&!waited&&b[18]==42);
 mode=1;calls=0;assert(x86bridge::Receive(pipe,peer,b,sizeof(b)));assert(calls==7&&waited==7);
 mode=2;assert(!x86bridge::Receive(pipe,peer,b,sizeof(b)));assert(cancelled==1&&retired==1&&lastError==ERROR_BROKEN_PIPE);
 mode=5;assert(!x86bridge::Receive(pipe,peer,b,sizeof(b)));assert(cancelled==2&&retired==2&&lastError==ERROR_TIMEOUT);
 mode=3;assert(!x86bridge::Receive(pipe,peer,b,sizeof(b)));
 printf("PASS IPC helper: partial I/O, bounded timeout, peer death, cancellation retirement and zero-byte rejection. Win32/GPU UNVALIDATED.\n");
}
''',encoding='utf-8')
 subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(p),'-I'+str(new),str(p/'io.cpp'),'-o',str(p/'io')],check=True)
 subprocess.run([str(p/'io')],check=True)
print('UNVALIDATED here: native MSVC/PE checks run in the Windows build; real ReShade/GPU/HIP/resize/crash tests remain manual.')

subprocess.run([__import__('sys').executable,str(root/'core/x86bridge/tests/test-x86bridge-v2.py')],check=True)

subprocess.run([__import__('sys').executable,str(root/'core/x86bridge/tests/test-x86bridge-factory.py')],check=True)
