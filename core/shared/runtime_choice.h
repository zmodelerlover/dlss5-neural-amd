#pragma once
// NrBackend in amd-nr.ini: which runtime runs the network. It is read once, when the network first
// starts, so a change applies on the next launch, as the OptiScaler fork's "NR runtime" does. The
// 64-bit add-on reads and writes it beside itself; on the 32-bit bridge the frontend writes it and
// the helper, which runs the network, reads the same file.
#include <windows.h>

#include <filesystem>

namespace runtime_choice {

enum : int { kDanielblnc = 0, kMochizuki = 1, kCount = 2 };

inline bool Installed(const std::filesystem::path& dir, int runtime);

// With no NrBackend, mochizuki when it is installed: the installer puts it in on RX 9000 cards only,
// where it is the default.
inline int Read(const std::filesystem::path& dir) {
    wchar_t v[32]{};
    GetPrivateProfileStringW(L"amd-nr", L"NrBackend", L"", v, 32, (dir / L"amd-nr.ini").c_str());
    if (v[0] == 0)
        return Installed(dir, kMochizuki) ? kMochizuki : kDanielblnc;
    return _wcsicmp(v, L"mochizuki") == 0 ? kMochizuki : kDanielblnc;
}

inline void Write(const std::filesystem::path& dir, int runtime) {
    WritePrivateProfileStringW(L"amd-nr", L"NrBackend",
                               runtime == kMochizuki ? L"mochizuki" : L"danielblnc",
                               (dir / L"amd-nr.ini").c_str());
}

// MochizukiNrRuntime.dll keeps dlssnr-amd\build.pending while it builds the network and takes it away when the
// build ends. Still there at the next start, the game went down during that build: on driver 32.0.32015 the
// driver's compiler crashes on one of mochizuki's shaders, every time. It becomes build.failed, so that the next
// build (picked again by hand) logs every shader it compiles, and true says this run takes danielblnc instead.
inline bool MochizukiBuildDied(const std::filesystem::path& dir) {
    const auto pending = dir / L"dlssnr-amd" / L"build.pending";
    if (GetFileAttributesW(pending.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    MoveFileExW(pending.c_str(), (dir / L"dlssnr-amd" / L"build.failed").c_str(), MOVEFILE_REPLACE_EXISTING);
    return true;
}

inline bool Installed(const std::filesystem::path& dir, int runtime) {
    std::error_code ec;
    if (runtime == kMochizuki)
        return std::filesystem::exists(dir / L"MochizukiNrRuntime.dll", ec) &&
               std::filesystem::exists(dir / L"dlssnr-amd" / L"dlssnr.bin", ec);
    return std::filesystem::exists(dir / L"dlssnr_amd_pass1.dll", ec);
}

} // namespace runtime_choice
