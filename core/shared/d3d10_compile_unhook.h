#pragma once
// ReShade exports D3D10CompileShader, as every d3d10.dll does, so it can stand in for one, and so
// it hooks the function of that name in both d3d10.dll and d3d10_1.dll. Calling through, it takes
// the original of the wrong one: d3d10_1's, which looks d3d10's up and calls it, which is hooked,
// and the two call each other until the stack runs out. Just Cause 2 compiles its shaders through
// d3d10_1 and died that way in ws2_32.dll, the guard page's neighbour, under ReShade 6.8.0 as
// dxgi.dll, with the add-on switched off; hostcheck's d3d10 host does the same.
//
// ReShade has no use for that function, so d3d10.dll's gets its own first bytes back, read from the
// file on disk: d3d10_1's lookup then reaches the compiler, and the hook on d3d10_1 still runs once
// on the way in. Called on init_device, since a game compiles only once its device is up, and only
// where d3d10_1.dll is in the process, the one that loops. The bytes put back hold no relocation
// in either width (a hot-patch prologue on x86, no absolute address on x64).
#include <windows.h>

#include <cstring>
#include <vector>

namespace d3d10unhook {

// The file laid out as the loader lays it out, unrelocated. LoadLibraryEx as an image resource
// would hand back the module already loaded, hooks and all.
inline bool ReadImage(const wchar_t* path, std::vector<unsigned char>& image) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    std::vector<unsigned char> raw;
    DWORD got = 0;
    const bool read = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20) &&
                      (raw.resize(static_cast<size_t>(size.QuadPart)), true) &&
                      ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &got, nullptr) &&
                      got == raw.size();
    CloseHandle(file);
    if (!read || raw.size() < sizeof(IMAGE_DOS_HEADER))
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > raw.size())
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    image.assign(nt->OptionalHeader.SizeOfImage, 0);
    const size_t headers = nt->OptionalHeader.SizeOfHeaders;
    if (headers > raw.size() || headers > image.size())
        return false;
    std::memcpy(image.data(), raw.data(), headers);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const size_t bytes = (std::min)(section->SizeOfRawData, section->Misc.VirtualSize);
        if (static_cast<size_t>(section->PointerToRawData) + bytes > raw.size() ||
            static_cast<size_t>(section->VirtualAddress) + bytes > image.size())
            return false;
        std::memcpy(image.data() + section->VirtualAddress, raw.data() + section->PointerToRawData, bytes);
    }
    return true;
}

// The RVA the export table gives `name` in a module laid out as an image at `base`, or 0.
inline DWORD ExportRva(const unsigned char* base, const char* name) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0)
        return 0;
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i)
        if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), name) == 0)
            return functions[ordinals[i]];
    return 0;
}

// What it did, for the log: nullptr when nothing was hooked.
inline const char* RestoreCompileShader() {
    if (GetModuleHandleW(L"d3d10_1.dll") == nullptr)
        return nullptr;
    // By full path, as d3d10_1.dll loads it on its first compile: that load is what ReShade installs
    // its delayed hooks on, whether or not the module is already in, and they have to be in place
    // to be undone.
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length + 11 > MAX_PATH)
        return nullptr;
    wcscat_s(path, L"\\d3d10.dll");
    HMODULE live = LoadLibraryW(path);
    std::vector<unsigned char> file;
    if (live == nullptr || !ReadImage(path, file))
        return nullptr;
    auto* base = reinterpret_cast<unsigned char*>(live);
    const DWORD rva = ExportRva(file.data(), "D3D10CompileShader");
    constexpr size_t kBytes = 16;
    if (rva == 0 || rva + kBytes > file.size() || ExportRva(base, "D3D10CompileShader") != rva)
        return nullptr;
    unsigned char* entry = base + rva;
    const unsigned char* original = file.data() + rva;
    if (std::memcmp(entry, original, kBytes) == 0)
        return nullptr;
    DWORD was = 0;
    if (!VirtualProtect(entry, kBytes, PAGE_EXECUTE_READWRITE, &was))
        return "could not restore d3d10.dll's D3D10CompileShader";
    std::memcpy(entry, original, kBytes);
    VirtualProtect(entry, kBytes, was, &was);
    FlushInstructionCache(GetCurrentProcess(), entry, kBytes);
    return "restored d3d10.dll's D3D10CompileShader, which ReShade hooks as well as d3d10_1.dll's and "
           "calls back in a loop";
}

}  // namespace d3d10unhook
