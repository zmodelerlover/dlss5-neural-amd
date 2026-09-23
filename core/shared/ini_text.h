#pragma once
// One repair, shared by the add-on and the bridge: a UTF-8 BOM in front of the ini.
//
// GetPrivateProfileString reads the file as bytes. A BOM puts EF BB BF in front of the first
// section header, which then matches no section, so every key in the file is invisible and every
// setting silently falls back to its built-in default -- no warning, nothing in the log, and a file
// that looks right in any editor. Windows PowerShell's `Set-Content -Encoding utf8` writes one, so
// a single hand edit of the ini is enough to lose the whole file. That is exactly what happened on
// 15/09/2026: the ini said ToggleKey=120 and the add-on kept reporting 35, which is the default.
//
// It is taken off in place, once, before the file is read.
#include <Windows.h>
#include <cstdio>
#include <string>

namespace ini_text {

// True when a BOM was found and removed. False means there was nothing to do, or the file could
// not be rewritten -- in which case reading it is no worse than it already was.
inline bool StripUtf8Bom(const std::wstring &ini)
{
    FILE *in = _wfopen(ini.c_str(), L"rb");
    if (in == nullptr)
        return false;

    unsigned char bom[3] {};
    const bool marked = std::fread(bom, 1, 3, in) == 3 &&
                        bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF;
    std::string rest;
    if (marked)
    {
        char buffer[4096];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), in)) > 0)
            rest.append(buffer, got);
    }
    std::fclose(in);
    if (!marked)
        return false;

    // Through a temporary and a replace, so an interrupted write cannot leave a settings file that
    // is half a settings file.
    const std::wstring temp = ini + L".bomfix";
    FILE *out = _wfopen(temp.c_str(), L"wb");
    if (out == nullptr)
        return false;
    const bool written = rest.empty() || std::fwrite(rest.data(), 1, rest.size(), out) == rest.size();
    std::fclose(out);
    if (!written || !MoveFileExW(temp.c_str(), ini.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

} // namespace ini_text
