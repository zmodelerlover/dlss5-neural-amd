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
// When the runtime moves again: re-derive, edit only this file, and run the checker.

#pragma once

#include <cstddef>

namespace rt
{

// -- Data ---------------------------------------------------------------------------------------

constexpr size_t kDevice = 0x96f68;          // ID3D12Device *, handed over before init
constexpr size_t kQueue = 0x96f70;           // ID3D12CommandQueue *, the present queue it records on
constexpr size_t kEngineObject = 0x96f78;    // the object kInitFn takes as its first argument
constexpr size_t kFrameCounter = 0x96fb0;    // engine+0x38: the evaluation count the pre-block kernel is
                                             // handed -- its noise seed. The worker bumps it after every
                                             // job; the record entry zeroes it after the warm-up job
constexpr size_t kSelfCheckFrame = 0x97058;  // engine+0xe0: the count on which the worker reads the
                                             // pre-block back for its zero-bytes self-check

constexpr size_t kHistory = 0x97090;         // ID3D12Resource *, last frame's output
constexpr size_t kHistoryOn = 0x97098;       // whether to read it

constexpr size_t kReady = 0x97298;           // set once init succeeded; the record entry tests it
constexpr size_t kNativeFailure = 0x9729a;   // the engine gave up; the record entry tests it

constexpr size_t kInlineMode = 0x977a0;      // 1 inline, 0 async. v0.3.0 reads the ini key `Async`,
                                             // which is this inverted; the byte itself did not change
constexpr size_t kInlineActive = 0x977a1;    // the engine's own verdict, latched only when it (re)creates
                                             // its staging (0x13b61/0x13bbf/0x141fc/0x165a1): 1 when
                                             // kInlineMode asked and zero-copy + the flag PSO came up
constexpr size_t kJobCounter = 0x977d4;      // interlocked; how far the engine has got -- same-frame
                                             // only. Its one non-zero store (0x1a2cb) sits behind the
                                             // worker's kInlineActive sample (jz at 0x1a1dd), so in
                                             // async it never moves; that is why async is retired.
                                             // Should it come back, idle is the record entry's own
                                             // admission (0x153dd-0x153f1): the dwords at 0x97a4c
                                             // (pending id) and 0x97a68 (recorded, not notified)
                                             // both < 0. Not 0x97a58 (last finished): the async
                                             // "readback map failed" drop (0x18ab0) skips it.

constexpr size_t kWaitBudgetMax = 0x978d0;   // int, InlineWaitMs as the engine took it: 200 when its
                                             // ini has none, clamped to 50..5000 (0x7eb0). Read only:
                                             // the ini is the person's. 0x978d4 is the budget the
                                             // worker lowers from it after timeouts
constexpr size_t kWatchdogJobA = 0x97950;    // a pair of job ids its watchdog writes on a timeout --
constexpr size_t kWatchdogJobB = 0x97954;    // NOT a pointer, and writing through it crashes
constexpr size_t kWatchdogFires = 0x97980;   // int, +1 each time that watchdog lets a job past the
                                             // budget go (0x1b297, same-frame only); zeroed when the
                                             // module recreates its staging (0x164ca)

constexpr size_t kInterop = 0x97984;
constexpr size_t kListMarker = 0x97a60;      // ID3D12CommandList *, the list it accepted
constexpr size_t kJobId = 0x97a6c;           // moves once per evaluation that really recorded.
                                             // kJobId - kJobCounter is the jobs in flight: the record
                                             // entry refuses at four on that subtraction (0x15383),
                                             // and the worker retires only a job it was notified of

// The option struct, mapped by decompiling the runtime's own ini reader: the key string sits beside
// the address it writes, so these are named rather than guessed.
constexpr size_t kDepthInverted = 0x97b10;   // int, the engine's own default of 1
constexpr size_t kFsrFlagsSeen = 0x97b14;
constexpr size_t kEnabled = 0x97b1c;         // ini `Enabled`
constexpr size_t kTemporal = 0x97b1d;        // ini `Temporal`
constexpr size_t kUseFsrInputs = 0x97b1e;    // ini `UseFsrInputs`
constexpr size_t kUseDepth = 0x97b1f;        // ini `UseDepth`
constexpr size_t kTonemap = 0x97b20;         // ini `Tonemap`
constexpr size_t kLocalTone = 0x97b30;       // ini `LocalTone`,       default 0.0
constexpr size_t kLocalStructure = 0x97b34;  // ini `LocalStructure`,  default 1.0
constexpr size_t kSkinStructure = 0x97b38;   // ini `SkinStructure`,   default -1.0
constexpr size_t kScale = 0x97b3c;           // ini `Scale`,           default 0.03125
constexpr size_t kUseAutoMask = 0x97b40;     // ini `UseAutoMask`,     default 1
constexpr size_t kToneChannels = 0x97b44;    // ini `ToneChannels`,    default 0
constexpr size_t kHipDevice = 0x97c30;       // the device actually in use, not the ini's copy

// The window the log dumps once after init, to show the engine's own defaults read back.
constexpr size_t kFloatDumpFirst = 0x97b28, kFloatDumpLast = 0x97b44;
constexpr size_t kByteDumpFirst = 0x97b10, kByteDumpLast = 0x97b27;

// -- Entry points -------------------------------------------------------------------------------

constexpr size_t kNotifyFn = 0x9460;         // the frame-notify the runtime would have detoured
constexpr size_t kRecordFn = 0x12640;        // records one evaluation onto a command list
constexpr size_t kInitFn = 0x1fe80;          // loads the weights

}  // namespace rt
