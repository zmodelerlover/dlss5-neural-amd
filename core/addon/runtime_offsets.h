// Every address this add-on writes into the DLSS-NR-on-AMD runtime, in one place.
//
// These are raw offsets into someone else's binary, re-derived for each build of it, and the file
// exists because they were once copied into four: neural.cpp, vk_route.inc, host64.cpp and
// framecheck.cpp. The move to v0.3.0 updated two of them. The other two kept the v0.2.17 job
// counter, 0x8d6f4, which in v0.3.0 lands in .rdata -- and the interlocked compare-exchange there
// is a write into read-only memory, so the 32-bit bridge's host died with 0xc0000005 on its first
// evaluation, every time, and the Vulkan route was carrying the same crash unfired. The stale
// notify entry beside it was worse than a crash: 0x9170 is still inside .text on v0.3.0, in the
// middle of an unrelated function, so it would have been called rather than faulted.
//
// Nothing here is a guess. `tools/runtime_offsets_check.py` reads this file, checks every address
// against the sections of the runtime it belongs to, decodes the record entry's first three tests,
// its in-flight test, the watchdog's counter and the InlineWaitMs store out of the instruction
// stream to prove they are the fields named below, and fails if any source file has gone back to
// writing a literal.
//
// v0.4.0 moved every one of them again, and not by one delta: every v0.3.0 address lands in .rdata
// on v0.4.0, the same crash as above waiting to happen. Each was re-derived twice, independently:
// once by aligning every matched function of v0.3.0 against v0.4.0 and letting each aligned
// reference vote, once by hand from an anchor in the new binary -- the ini reader's key string
// beside its store, a log format string that labels the argument, the record gate, the init call
// site. The option struct keeps its shape but moved by 0x10ae8 up to ToneChannels and by 0x10af8
// after it, where v0.3.3 inserted Style, ToneCurve, ToneLift and UseGameExposure. The scripts and
// their output are kept outside the repository, in daniel-runtime/analysis-reshade (map-data-state,
// map-options-code, map-patches, reconcile; implement/anchor_check.py replays one anchor per
// address against this file).
//
// v0.4.1 moved the data block again, in two pieces: +0x2020 up to kHistoryOn and +0x2038 from kReady
// on. Init, notify and the ini reader's stores kept their code; the record entry gained the
// QueuePriority stream. Mapped twice from v0.4.0 (unique instruction windows, and the aligned
// references of every matched function), in daniel-runtime/analysis-opti (map_layout_040_041.txt,
// datamap_040_041.txt), and the two agree on every address here. Code addresses quoted in the
// comments below (worker 0x1be8f and so on) are still v0.4.0's, except beside kFrameCounter,
// kSelfCheckFrame, kInlineActive, kJobCounter, kWaitBudgetMax, kWatchdogFires and kJobId: those
// were mapped straight from v0.3.0 to v0.4.1 by the anchors quoted there, and are v0.4.1's.
//
// v0.4.2 moved the data block in three pieces: +0x51c0 up to kHistoryOn, +0x51d0 from kReady to
// kWatchdogFires, +0x51d8 from kCpuWait on; the engine members this add-on writes kept engine+0x3c
// and +0x104. Mapped twice, with no delta taken as proof: by the anchor of each address in the
// v0.4.2 binary (integracao-v042/ida/REMAP-v0.4.2.md, outside the repository), and against the
// OptiScaler fork's own layout (AmdLayout.h, kAmd042), which agree on every field both name. The
// two only this add-on uses were derived again on their own: kCpuWait from the ini reader's store
// (now a checker rule), kNetworkMs from the worker's write and the log's read, the same pair of
// instructions 0x108 apart on both builds. It adds two ini keys: Quality, which the add-on writes
// from its own setting (kQuality), and NoiseHandoff, which it leaves alone (see seed.inc).
// Code addresses quoted in the comments below are v0.4.1's.
//
// 0.5.0 is danielblnc's supporter build, and is not distributed: whoever has it supplies their own
// file. It inserted 0x10 bytes near the start of the engine object (kFrameCounter is engine+0x4c,
// kSelfCheckFrame +0x114) and moved the data block again. Mapped the same two ways from v0.4.2,
// agreeing on every field both name. Its own overlay (OverlayKey) is drawn only from the Present
// detours the setup thread installs, which the first patch removes, as on every build; the half-
// precision weight copy it keeps is made on RDNA3 only (about 280 MB more per module there).
//
// v0.4.3, public since 2026-09-28, sits between the two: the engine side is v0.4.2's (kFrameCounter
// engine+0x3c, kSelfCheckFrame +0x104; one 8-byte member inserted between +0x150 and +0x180, where
// none of these are), and the host side takes 0.5.0's ini reader, worker and static init, OverlayKey
// with them. The data block moved again (+0x2170 up to kHistoryOn, +0x2178 from kReady on). Mapped
// twice from the binary, once from v0.4.2 and once from 0.5.0, agreeing on every address with the same
// reference count on all three builds (integracao-v043/ida/REMAP-v0.4.3.md, outside the repository),
// and against the OptiScaler fork's own layout (AmdLayout.h, kAmd043), which agrees on every field
// both name. Its overlay is reached only from the Present detours the setup thread installs, as on
// 0.5.0, so the first patch keeps it off too.
//
// 0.5.1 is danielblnc's next supporter build, not distributed either. The engine object is 0.5.0's
// up to every member these name (kFrameCounter engine+0x4c, kSelfCheckFrame +0x114; members from +0x4e0
// on moved +0x88, where none of these are). The data block moved +0x3010 up to kHistoryOn and +0x3098
// from kReady on; Init moved, Notify, Record and both patch sites did not. Mapped from 0.5.0 by
// aligned instructions, one target per field, and against the OptiScaler fork's kAmd051.
//
// 0.6.0 is danielblnc's next supporter build (adds RDNA2), not distributed either. The engine object
// moved members: kFrameCounter stays engine+0x4c, kSelfCheckFrame is +0x128 (the worker's compare at
// 0x3bdd8). The data block moved +0x5060 to +0x50e8, and the option bytes from kEnabled on are spread
// out (the byte dump window is 36 bytes now). Init, Notify and Record all moved, and so did both patch
// sites (file 0x64ed and 0xa8c2). Mapped from 0.5.1 by aligned instructions, one target per field, and
// against the OptiScaler fork's kAmd060.
//
// When the runtime moves again: add its build to kBuilds (runtime_builds.inc) and its patches to runtime-patches.json,
// and run the checker against the patched file.

#pragma once

#include <cstddef>

namespace rt
{

struct Build
{
// The DLSS-NR-on-AMD release, for the panel, and the patched file the add-on loads: its size and
// SHA-256, which is how IdentifyRuntime tells the builds apart.
const char *kVersion;
size_t kSize;
const char *kSha256;

// -- Data ---------------------------------------------------------------------------------------

size_t kDevice;                           // ID3D12Device *, handed over before init
size_t kQueue;                            // ID3D12CommandQueue *, the present queue it records on
size_t kEngineObject;                     // the object kInitFn takes as its first argument
size_t kFrameCounter;                       // engine+0x3c (+0x38 on v0.3.0; that is a float now):
                                            // the evaluation count the pre-block kernel is handed --
                                            // its noise seed. The launch function bumps it after
                                            // every job (0x3803d); the record entry zeroes it after
                                            // the warm-up job (0x172c6)
size_t kSelfCheckFrame;                     // engine+0x104 (+0xe0 on v0.3.0): the count on which
                                            // the worker reads the pre-block back for its zero-bytes
                                            // self-check (cmp at 0x35142; seeded at 0x24e22)

size_t kHistory;                       // ID3D12Resource *, last frame's output
size_t kHistoryOn;                     // whether to read it

size_t kReady;                             // set once init succeeded; the record entry tests it
size_t kNativeFailure;                     // the engine gave up; the record entry tests it

size_t kInlineMode;                     // 1 inline, 0 async. The runtime reads the ini key
                                        // `Async`, which is this inverted
size_t kInlineActive;                     // the engine's own verdict, latched only when it
                                          // (re)creates its staging (0x1605d/0x160bb/0x1669c/
                                          // 0x166d1/0x18d00): 1 when kInlineMode asked and zero-copy
                                          // + the flag PSO came up. The record entry starts its
                                          // watchdog only on 1 (0x1783d)
size_t kJobCounter;                     // interlocked; how far the engine has got -- same-frame
                                        // only. Its one non-zero store, the worker's retire
                                        // (0x1d455), sits behind the worker's inline sample (jz at
                                        // 0x1d363), so in async it never moves; that is why async
                                        // is retired. The async idle test v0.3.0 had is not mapped.

// float, the network's GPU time for the job that just finished, in ms: the "X ms network on the
// GPU" of the runtime's "network job N done" log line (its fourth argument, read at 0x1de20). The
// worker writes it when a job ends (0x1dd18) and nowhere else. The float beside it, 0xaa2ac, is
// the line's "waiting for the capture" and is zeroed when a job starts.
size_t kNetworkMs;                    

size_t kWaitBudgetMax;                     // int, InlineWaitMs as the engine took it: 200 when its
                                           // ini has none, clamped to 50..5000 (store at 0x855c).
                                           // Read only: the ini is the person's. 0xaa39c is the
                                           // budget the worker lowers from it after timeouts
size_t kWatchdogJobA;                     // a pair of job ids its watchdog writes on a timeout --
size_t kWatchdogJobB;                     // NOT a pointer, and writing through it crashes
size_t kWatchdogFires;                     // int, +1 each time that watchdog lets a job past the
                                           // budget go (0x1f587, same-frame only); zeroed when the
                                           // module recreates its staging (0x18c29)

// ini `CpuWait`, new in v0.3.1: 1 makes the notify entry block the calling thread until the job
// it just took has finished, up to twice InlineWaitMs; 2, the default, only within a second of an
// FSR frame-generation dispatch, which cannot happen here. 0 never waits. Pinned to 0: every
// notify comes from the thread presenting the game's frame, the add-on paces the network itself
// (RuntimeBusy), and v0.3.0 had no such wait at all.
size_t kCpuWait;                    

size_t kInterop;                    
size_t kListMarker;                     // ID3D12CommandList *, the list it accepted
size_t kJobId;                          // moves once per evaluation that really recorded.
                                        // kJobId - kJobCounter is the jobs in flight: the record
                                        // entry refuses at four on that subtraction (0x17928), and
                                        // the worker retires only a job it was notified of

// The option struct, mapped by decompiling the runtime's own ini reader: the key string sits beside
// the address it writes, so these are named rather than guessed.
size_t kDepthInverted;                     // int, the engine's own default of 1
size_t kFsrFlagsSeen;                    
size_t kEnabled;                            // ini `Enabled`
size_t kTemporal;                           // ini `Temporal`
size_t kUseFsrInputs;                       // ini `UseFsrInputs`
size_t kUseDepth;                           // ini `UseDepth`
size_t kTonemap;                            // ini `Tonemap`
size_t kLocalTone;                          // ini `LocalTone`,       default 0.0
size_t kLocalStructure;                     // ini `LocalStructure`,  default 1.0
size_t kSkinStructure;                      // ini `SkinStructure`,   default -1.0
size_t kScale;                              // ini `Scale`,           default 0.03125
size_t kUseAutoMask;                        // ini `UseAutoMask`,     default 1
size_t kToneChannels;                       // ini `ToneChannels`,    default 0

// Inserted by v0.3.3 and pinned to their defaults, because the runtime reads them from an ini that
// the standalone runtime's own overlay writes them back into, so a game folder that once had it
// keeps them. Style goes to the network as Style/128, into the input v0.3.0 held at zero (worker
// 0x1be8f); ToneCurve and ToneLift reshape the apply pass's tonemap when Tonemap is on (worker
// 0x1be70, 0x1be7c). Measured with framecheck, the pins taken out: Style=2 moved the output by a
// mean 0.008, ToneCurve=aces with ToneLift=0.25 by 0.009; pinned, an ini with both gives the same
// bytes as the add-on's own. UseGameExposure, the fourth, is not pinned: the record entry honours
// it only when the packet carries an exposure texture (0x17e22), and this add-on passes none; a
// framecheck run with it at 0 gave the same bytes.
size_t kStyle;                         // ini `Style`, int, clamped to 0..2
size_t kToneCurve;                     // ini `ToneCurve`, int: 0 reinhard, 1 aces
size_t kToneLift;                      // ini `ToneLift`, float, clamped to 0..0.25

// ini `Quality`, new in v0.4.2 and read as a string: `fast` in any case, or no key at all, stores
// 1, the fast kernels (f32 accumulation, approximate rsqrt and rcp; upstream measures them about
// 13-15% faster on RX 9000, and the difference is barely visible); anything else 0, NVIDIA's exact
// arithmetic. The worker copies it into the engine on every job, so a write applies from the next
// one. On v0.4.2 a GPU without the fast kernels runs reference whatever it says; v0.4.3 and 0.5.0
// push it to the device on every job (g_fastpow), and v0.4.3 logs that the colour conversion takes
// the fast path even there; on 0.5.0 and v0.4.3 a change also resets the runtime's own temporal
// history. The add-on writes its own Quality here every frame, over the runtime's ini. The ini
// reader's `sete` stores it (0x89d6 on v0.4.2, 0x8d1e on v0.4.3, 0x8b1e on 0.5.0), and the checker
// decodes that store.
size_t kQuality;                       // optional: 0 on a build without the key (0.4.1). byte

size_t kHipDevice;                     // the device actually in use, not the ini's copy

// The window the log dumps once after init, to show the engine's own defaults read back.
size_t kFloatDumpFirst, kFloatDumpLast;                              
size_t kByteDumpFirst, kByteDumpLast;                              

// -- Entry points -------------------------------------------------------------------------------

size_t kNotifyFn;                     // the frame-notify the runtime would have detoured
size_t kRecordFn;                     // records one evaluation onto a command list
size_t kInitFn;                       // loads the weights
};

#include "runtime_builds.inc"

// The build in use, from the moment IdentifyRuntime has recognised dlssnr_amd_pass1.dll; null
// before, so an address read too early faults at once instead of writing at the module's base.
inline const Build *B = nullptr;

}  // namespace rt
