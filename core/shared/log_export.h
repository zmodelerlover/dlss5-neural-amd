#pragma once
// Copies a run's logs and the settings that produced them to a dated folder on the desktop.
// Shared by the 64-bit add-on and the 32-bit bridge, which differ only in which files there are
// and where they live: the bridge has two logs, and the frontend writes beside the add-on while
// the helper writes beside the game, so each name is looked for in every place given.
//
// Every question worth asking about a run needs the same few files, and asking somebody to find
// them means asking them to find the emulator's install directory first. The ini goes with them
// because a log without the settings that produced it cannot be compared against anything.
//
// SHGetKnownFolderPath rather than %USERPROFILE%\Desktop: a desktop redirected into OneDrive is
// ordinary now, and the guessed path would silently write somewhere nobody looks.
//
// A log still open while this runs is copied up to this moment: both routes open theirs through
// the CRT, which shares for reading, and flush every line as it is written.
#include <windows.h>
#include <shlobj.h>

#include <cwchar>
#include <filesystem>
#include <initializer_list>

namespace logexport {

struct Result
{
    std::filesystem::path folder;  // empty when nothing could be written
    int copied = 0;
};

inline Result ToDesktop(std::initializer_list<std::filesystem::path> places,
                        std::initializer_list<const wchar_t *> names)
{
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        return {};
    std::filesystem::path out(desktop);
    CoTaskMemFree(desktop);

    wchar_t stamp[32] {};
    SYSTEMTIME now {};
    GetLocalTime(&now);
    std::swprintf(stamp, 32, L"amd-nr-logs-%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth,
                  now.wDay, now.wHour, now.wMinute, now.wSecond);
    out /= stamp;

    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    if (ec)
        return {};

    int copied = 0;
    for (const wchar_t *name : names)
        for (const std::filesystem::path &dir : places)
        {
            const std::filesystem::path from = dir / name;
            if (!std::filesystem::exists(from, ec))
                continue;
            std::filesystem::copy_file(from, out / name,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec)
            {
                ++copied;
                break;
            }
        }
    if (copied == 0)
    {
        std::filesystem::remove(out, ec);
        return {};
    }
    return { out, copied };
}

} // namespace logexport
