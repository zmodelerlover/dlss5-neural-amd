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
// against the sections of the runtime it belongs to, decodes the record entry's first three tests
// out of the instruction stream to prove they are the fields named below, and fails if any source
// file has gone back to writing a literal.
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
// comments below (worker 0x1be8f and so on) are still v0.4.0's.
//
// When the runtime moves again: re-derive, edit only this file, and run the checker.

#pragma once

#include <cstddef>

namespace rt
{

// The DLSS-NR-on-AMD release kRuntimeSha256 names, for the panel.
constexpr char kVersion[] = "0.4.1";

// -- Data ---------------------------------------------------------------------------------------

constexpr size_t kDevice = 0xa98e0;       // ID3D12Device *, handed over before init
constexpr size_t kQueue = 0xa98e8;        // ID3D12CommandQueue *, the present queue it records on
constexpr size_t kEngineObject = 0xa98f8; // the object kInitFn takes as its first argument

constexpr size_t kHistory = 0xa9a40;   // ID3D12Resource *, last frame's output
constexpr size_t kHistoryOn = 0xa9a48; // whether to read it

constexpr size_t kReady = 0xa9d48;         // set once init succeeded; the record entry tests it
constexpr size_t kNativeFailure = 0xa9d4a; // the engine gave up; the record entry tests it

constexpr size_t kInlineMode = 0xaa250; // 1 inline, 0 async. The runtime reads the ini key
                                        // `Async`, which is this inverted
constexpr size_t kJobCounter = 0xaa284; // interlocked; how far the engine has got

// float, the network's GPU time for the job that just finished, in ms: the "X ms network on the
// GPU" of the runtime's "network job N done" log line (its fourth argument, read at 0x1de20). The
// worker writes it when a job ends (0x1dd18) and nowhere else. The float beside it, 0xaa2ac, is
// the line's "waiting for the capture" and is zeroed when a job starts.
constexpr size_t kNetworkMs = 0xaa2b0;

constexpr size_t kWatchdogJobA = 0xaa418; // a pair of job ids its watchdog writes on a timeout --
constexpr size_t kWatchdogJobB = 0xaa41c; // NOT a pointer, and writing through it crashes

// ini `CpuWait`, new in v0.3.1: 1 makes the notify entry block the calling thread until the job
// it just took has finished, up to twice InlineWaitMs; 2, the default, only within a second of an
// FSR frame-generation dispatch, which cannot happen here. 0 never waits. Pinned to 0: every
// notify comes from the thread presenting the game's frame, the add-on paces the network itself
// (RuntimeBusy), and v0.3.0 had no such wait at all.
constexpr size_t kCpuWait = 0xaa478;

constexpr size_t kInterop = 0xaa4a0;
constexpr size_t kListMarker = 0xaa580; // ID3D12CommandList *, the list it accepted
constexpr size_t kJobId = 0xaa58c;      // moves once per evaluation that really recorded

// The option struct, mapped by decompiling the runtime's own ini reader: the key string sits beside
// the address it writes, so these are named rather than guessed.
constexpr size_t kDepthInverted = 0xaa630; // int, the engine's own default of 1
constexpr size_t kFsrFlagsSeen = 0xaa634;
constexpr size_t kEnabled = 0xaa63c;        // ini `Enabled`
constexpr size_t kTemporal = 0xaa63d;       // ini `Temporal`
constexpr size_t kUseFsrInputs = 0xaa63e;   // ini `UseFsrInputs`
constexpr size_t kUseDepth = 0xaa63f;       // ini `UseDepth`
constexpr size_t kTonemap = 0xaa640;        // ini `Tonemap`
constexpr size_t kLocalTone = 0xaa650;      // ini `LocalTone`,       default 0.0
constexpr size_t kLocalStructure = 0xaa654; // ini `LocalStructure`,  default 1.0
constexpr size_t kSkinStructure = 0xaa658;  // ini `SkinStructure`,   default -1.0
constexpr size_t kScale = 0xaa65c;          // ini `Scale`,           default 0.03125
constexpr size_t kUseAutoMask = 0xaa660;    // ini `UseAutoMask`,     default 1
constexpr size_t kToneChannels = 0xaa664;   // ini `ToneChannels`,    default 0

// Inserted by v0.3.3 and pinned to their defaults, because the runtime reads them from an ini that
// the standalone runtime's own overlay writes them back into, so a game folder that once had it
// keeps them. Style goes to the network as Style/128, into the input v0.3.0 held at zero (worker
// 0x1be8f); ToneCurve and ToneLift reshape the apply pass's tonemap when Tonemap is on (worker
// 0x1be70, 0x1be7c). Measured with framecheck, the pins taken out: Style=2 moved the output by a
// mean 0.008, ToneCurve=aces with ToneLift=0.25 by 0.009; pinned, an ini with both gives the same
// bytes as the add-on's own. UseGameExposure, the fourth, is not pinned: the record entry honours
// it only when the packet carries an exposure texture (0x17e22), and this add-on passes none; a
// framecheck run with it at 0 gave the same bytes.
constexpr size_t kStyle = 0xaa668;     // ini `Style`, int, clamped to 0..2
constexpr size_t kToneCurve = 0xaa66c; // ini `ToneCurve`, int: 0 reinhard, 1 aces
constexpr size_t kToneLift = 0xaa670;  // ini `ToneLift`, float, clamped to 0..0.25

constexpr size_t kHipDevice = 0xaa760; // the device actually in use, not the ini's copy

// The window the log dumps once after init, to show the engine's own defaults read back.
constexpr size_t kFloatDumpFirst = 0xaa648, kFloatDumpLast = 0xaa664;
constexpr size_t kByteDumpFirst = 0xaa630, kByteDumpLast = 0xaa647;

// -- Entry points -------------------------------------------------------------------------------

constexpr size_t kNotifyFn = 0x9d80;  // the frame-notify the runtime would have detoured
constexpr size_t kRecordFn = 0x14c40; // records one evaluation onto a command list
constexpr size_t kInitFn = 0x26130;   // loads the weights

}  // namespace rt
