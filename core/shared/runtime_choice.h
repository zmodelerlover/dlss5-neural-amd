#pragma once
// NrBackend in amd-nr.ini: which runtime runs the network. It is read once, when the network first
// starts, so a change applies on the next launch, as the OptiScaler fork's "NR runtime" does. The
// 64-bit add-on reads and writes it beside itself; on the 32-bit bridge the frontend writes it and
// the helper, which runs the network, reads the same file.
#include <windows.h>

#include <filesystem>

namespace runtime_choice {

enum : int { kDanielblnc = 0, kMochizuki = 1, kCount = 2 };

inline int Read(const std::filesystem::path& dir) {
    wchar_t v[32]{};
    GetPrivateProfileStringW(L"amd-nr", L"NrBackend", L"", v, 32, (dir / L"amd-nr.ini").c_str());
    return _wcsicmp(v, L"mochizuki") == 0 ? kMochizuki : kDanielblnc;
}

inline void Write(const std::filesystem::path& dir, int runtime) {
    WritePrivateProfileStringW(L"amd-nr", L"NrBackend",
                               runtime == kMochizuki ? L"mochizuki" : L"danielblnc",
                               (dir / L"amd-nr.ini").c_str());
}

inline bool Installed(const std::filesystem::path& dir, int runtime) {
    std::error_code ec;
    if (runtime == kMochizuki)
        return std::filesystem::exists(dir / L"MochizukiNrRuntime.dll", ec) &&
               std::filesystem::exists(dir / L"dlssnr-amd" / L"dlssnr.bin", ec);
    return std::filesystem::exists(dir / L"dlssnr_amd_pass1.dll", ec);
}

} // namespace runtime_choice
