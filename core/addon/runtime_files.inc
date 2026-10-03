// The runtime's file before it loads: which build it is, its own ini, and the copy that imports
// the private D3D12 instead of the system one; the HIP device it will run on; and the probe that
// says who called into null once it runs. Included by neural.cpp where they used to be.

// The engine reads dlssnr_on_amd.ini from DllMain, so it has to exist before LoadLibrary.
// Without the key its InlineWaitMs is 200 ms, clamped to 50..5000 (ini reader, v0.3.0 and v0.4.0
// alike). A fresh one halves that: a stuck job holds the game's queue that long every frame, and a
// stall that runs into seconds is Windows TDR, which removes the D3D12 device and takes the game
// with it (DXGI_ERROR_DEVICE_REMOVED, 887A0005). One that exists is the person's and is never
// rewritten; InitEngine logs the InlineWaitMs it set, and says so when it is over 200.
void EnsureEngineIni(const std::filesystem::path &dir)
{
    const auto ini = dir / L"dlssnr_on_amd.ini";
    std::error_code ec;
    if (std::filesystem::exists(ini, ec))
        return;

    std::ofstream f(ini, std::ios::binary);
    if (!f)
    {
        Log("could not write %ls; the engine will fall back to its own defaults.", ini.c_str());
        return;
    }
    // v0.3.0 renamed the inline switch: the key is now `Async`, and it is the inverse of the old
    // `Inline` (Async=0 means inline). An ini this add-on wrote for v0.2.17 is still correct by
    // accident -- the unknown `Inline` key is ignored and `Async` defaults to 0 -- so an existing
    // file is left alone, as it always was.
    f << "[DlssNrOnAmd]\r\n"
         "Enabled=1\r\n"
         "Async=0\r\n"
         "; Host watchdog budget, ms (the engine's own default is 200; it accepts 50 to 5000).\r\n"
         "; The network takes about 16 ms at 0.50 scale, so 100 is a wide margin; past it the\r\n"
         "; engine stops waiting and the game goes on instead of freezing. Keep it far under\r\n"
         "; 2000: a stall that long is a Windows driver reset (TDR).\r\n"
         "InlineWaitMs=100\r\n"
         "Interop=1\r\n"
         "UseFsrInputs=1\r\n"
         "UseDepth=0\r\n"
         "Temporal=0\r\n"
         "Tonemap=0\r\n"
         "HipDevice=-1\r\n";
    Log("wrote a default dlssnr_on_amd.ini next to the exe.");
}

// Up to v0.3.0 the NR runtime imported d3d12.dll statically, so merely loading it pulled the
// system copy into the process -- and that is the thing this whole dance exists to avoid.
// Measured: with the engine loaded, ReShade installs its d3d12 hooks and the game's next resize
// dies; without it, no hooks and the resizes pass. Since v0.3.1 it is delay-loaded, which only
// moves the moment: the first record reaches D3D12SerializeRootSignature, and the delay helper
// calls LoadLibraryExA("d3d12.dll", 0, 0) -- the system copy again, mid-game. On v0.4.0 the name
// is in the delay-import table alone, and a walk of the import table found nothing to rewrite.
//
// An import is just a null-terminated name in the file, so a copy with that name overwritten by
// one of the same length imports whatever we choose, through either table. The private D3D12 is
// already loaded by the time this runs, so the loader matches it by base name and never goes to
// disk for it.
//
// The original file is left alone, and it is the original that the hash is checked against.
std::filesystem::path RuntimeCopyUsingPrivateD3D12(const std::filesystem::path &original)
{
    if (g_privateD3D12 == nullptr)
        return original;  // the game is on D3D12 already; its d3d12.dll is long since loaded

    const auto patched = ExeDirectory() / L"amd-nr-pass1.dll";
    std::ifstream in(original, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    in.close();
    if (bytes.empty())
        return original;

    // Rewrite ONLY the name in the import tables. Overwriting every occurrence of the string in
    // the file was wrong: the same bytes can appear inside code or unrelated data, and patching
    // those corrupts the DLL. The runtime then faulted repeatedly (0xC0000005 at heap addresses)
    // and the frame came back black. Walk the PE properly instead.
    auto rvaToOffset = [&](uint32_t rva) -> size_t {
        if (bytes.size() < 0x40)
            return 0;
        const auto u16 = [&](size_t o) {
            return static_cast<uint16_t>((unsigned char)bytes[o] | ((unsigned char)bytes[o + 1] << 8));
        };
        const auto u32 = [&](size_t o) {
            return static_cast<uint32_t>((unsigned char)bytes[o] | ((unsigned char)bytes[o + 1] << 8) |
                                         ((unsigned char)bytes[o + 2] << 16) |
                                         ((unsigned char)bytes[o + 3] << 24));
        };
        const size_t pe = u32(0x3C);
        if (pe + 24 > bytes.size() || u32(pe) != 0x00004550)
            return 0;
        const uint16_t sections = u16(pe + 6);
        const uint16_t optSize = u16(pe + 20);
        size_t sec = pe + 24 + optSize;
        for (uint16_t i = 0; i < sections; ++i, sec += 40)
        {
            if (sec + 40 > bytes.size())
                return 0;
            const uint32_t va = u32(sec + 12), rawSize = u32(sec + 16), rawPtr = u32(sec + 20);
            if (rva >= va && rva < va + rawSize)
                return rawPtr + (rva - va);
        }
        return 0;
    };
    const auto u16at = [&](size_t o) {
        return static_cast<uint16_t>((unsigned char)bytes[o] | ((unsigned char)bytes[o + 1] << 8));
    };
    const auto u32at = [&](size_t o) {
        return static_cast<uint32_t>((unsigned char)bytes[o] | ((unsigned char)bytes[o + 1] << 8) |
                                     ((unsigned char)bytes[o + 2] << 16) |
                                     ((unsigned char)bytes[o + 3] << 24));
    };
    size_t rewritten = 0;
    const auto pointAtPrivate = [&](uint32_t nameRva) {
        constexpr size_t n = sizeof(kSystemD3D12Ansi) - 1;
        const size_t nameOff = rvaToOffset(nameRva);
        if (nameOff == 0 || nameOff + n >= bytes.size() ||
            _strnicmp(&bytes[nameOff], kSystemD3D12Ansi, n) != 0 || bytes[nameOff + n] != 0)
            return;
        std::memcpy(&bytes[nameOff], kPrivateD3D12Ansi, n);
        ++rewritten;
    };
    if (bytes.size() > 0x40)
    {
        const size_t pe = u32at(0x3C);
        if (pe + 24 < bytes.size() && u32at(pe) == 0x00004550)
        {
            const size_t opt = pe + 24;
            const uint16_t magic = u16at(opt);
            // The import directory is entry 1 of the data directories, which sit after the
            // fixed part of the optional header: 96 bytes for PE32, 112 for PE32+. A null name
            // ends each array.
            const size_t dirs = opt + (magic == 0x20B ? 112 : 96);
            if (dirs + 14 * 8 <= bytes.size()) {
                for (size_t desc = rvaToOffset(u32at(dirs + 1 * 8));
                     desc != 0 && desc + 20 <= bytes.size() && u32at(desc + 12) != 0; desc += 20)
                    pointAtPrivate(u32at(desc + 12));
                // The delay-import directory is entry 13: 32-byte descriptors, name at +4, and
                // bit 0 of the first word set when the fields are RVAs rather than addresses.
                const bool hasDelay = u32at(dirs - 4) > 13;
                for (size_t desc = hasDelay ? rvaToOffset(u32at(dirs + 13 * 8)) : 0;
                     desc != 0 && desc + 32 <= bytes.size() && u32at(desc + 4) != 0; desc += 32)
                    if ((u32at(desc) & 1) != 0)
                        pointAtPrivate(u32at(desc + 4));
            }
        }
    }
    if (rewritten == 0)
    {
        Log("no d3d12.dll import found in the runtime; loading it unpatched.");
        return original;
    }

    std::ofstream out(patched, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        Log("could not write %ls; loading the runtime unpatched, which will pull the system "
            "d3d12.dll in.", patched.c_str());
        return original;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    Log("runtime copied to %ls with its %s import pointed at %s (%zu occurrence(s)).",
        patched.filename().c_str(), kSystemD3D12Ansi, kPrivateD3D12Ansi, rewritten);
    return patched;
}

// Every build of DLSS-NR-on-AMD this add-on can drive is in runtime_offsets.h, each named by the
// hash of its patched file: lifted out of its setup by tools/extract_runtime.py and run through
// tools/patch_runtime.py. Anything else is refused by hash rather than written into with the wrong
// addresses.

// The builds this add-on knows, for the log: "0.4.1, 0.4.2, 0.4.3, 0.5.0, 0.5.1, 0.6.0".
std::string KnownRuntimes()
{
    std::string out;
    for (const auto &b : rt::kBuilds)
        out += (out.empty() ? "" : ", ") + std::string(b.kVersion);
    return out;
}

// Which build `file` is, by size and SHA-256, and from then on the one every address goes
// through (rt::B); null for one this add-on does not know.
const rt::Build *IdentifyRuntime(const std::filesystem::path &file)
{
    // The runtime is a fixed-size binary per build, so anything of another size is the wrong file
    // -- and that is knowable from the directory entry. Reading it in to find out pulls the whole
    // of whatever was pointed at into memory first, which is a strange way to reject a wrong DLL.
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(file, sizeError);
    if (sizeError)
        return nullptr;
    // Say which file was rejected and what it is, not just that something was. Every offset in
    // InitEngine is a raw write into that DLL's globals, so accepting a build that is not in the
    // table is not an option; saying plainly which builds are wanted is.
    if (std::none_of(std::begin(rt::kBuilds), std::end(rt::kBuilds),
                     [&](const rt::Build &b) { return b.kSize == size; }))
    {
        Log("dlssnr_amd_pass1.dll is %llu bytes, the size of no danielblnc build this add-on knows "
            "(%s), and every address it writes belongs to one of those. Refused.",
            static_cast<unsigned long long>(size), KnownRuntimes().c_str());
        return nullptr;
    }
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    if (data.size() != size)
        return nullptr;
    BCRYPT_ALG_HANDLE alg {};
    unsigned char digest[32] {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return nullptr;
    const auto result =
        BCryptHash(alg, nullptr, 0, data.data(), static_cast<ULONG>(data.size()), digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (result < 0)
        return nullptr;
    char hex[65] {};
    for (int i = 0; i < 32; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    for (const auto &b : rt::kBuilds)
        if (b.kSize == size && std::strcmp(hex, b.kSha256) == 0)
        {
            rt::B = &b;
            Log("runtime: danielblnc %s (%.16s...)", b.kVersion, hex);
            return rt::B;
        }
    Log("dlssnr_amd_pass1.dll is the size of a known build but not one: SHA-256 starts %.16s..., and "
        "this add-on knows %s. Refused.", hex, KnownRuntimes().c_str());
    return nullptr;
}

// Who jumped to null, and from where.
//
// A call through a null pointer faults with ExceptionAddress == 0, and the engine's own handler
// prints exactly that and nothing else, which names the victim and not the culprit. The return
// address a `call` pushes is still sitting at RSP, so one read of it says which instruction in
// the runtime made the call, as an offset from the module base -- and that is a line in IDA.
//
// First-chance and read-only: this returns CONTINUE_SEARCH always, so it changes no behaviour.
PVOID g_probe = nullptr;  // removed at DETACH: an unload mid-process must not leave it dangling
LONG CALLBACK NullJumpProbe(EXCEPTION_POINTERS *e)
{
    static LONG reported = 0;
    if (e == nullptr || e->ExceptionRecord == nullptr || e->ContextRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        e->ExceptionRecord->ExceptionAddress != nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&reported, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t rsp = static_cast<uintptr_t>(e->ContextRecord->Rsp);
    for (int i = 0; i < 8; ++i)
    {
        uintptr_t slot = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(rsp + i * 8), &slot,
                              sizeof(slot), nullptr) == 0)
            continue;
        HMODULE owner {};
        char name[MAX_PATH] {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(slot), &owner) &&
            GetModuleFileNameA(owner, name, MAX_PATH) != 0)
        {
            const char *leaf = std::strrchr(name, '\\');
            Log("fault probe: jumped to null; stack+%d returns to %s+0x%llx%s", i * 8,
                leaf ? leaf + 1 : name,
                static_cast<unsigned long long>(slot - reinterpret_cast<uintptr_t>(owner)),
                owner == g.runtime ? "   <<< THE RUNTIME" : "");
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool InitHip()
{
    if (g.hipSet != nullptr)
        return true;
    HMODULE hip = LoadLibraryExW(L"amdhip64_7.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    DWORD hipError = hip == nullptr ? GetLastError() : 0;
    // RX 6000 drivers ship HIP 6 only; the AMD HIP SDK brings HIP 7, sets HIP_PATH and leaves PATH alone. Loaded
    // here by full path, the runtime's own load by name finds the same module.
    wchar_t hipPath[MAX_PATH];
    const DWORD hipPathLength = GetEnvironmentVariableW(L"HIP_PATH", hipPath, MAX_PATH);
    if (hip == nullptr && hipPathLength > 0 && hipPathLength < MAX_PATH)
    {
        hip = LoadLibraryExW((std::filesystem::path(hipPath) / L"bin" / L"amdhip64_7.dll").c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (hip == nullptr)
            hipError = GetLastError();
        else
            Log("HIP: amdhip64_7.dll from HIP_PATH (%ls)", hipPath);
    }
    if (hip == nullptr)
    {
        Log("HIP: amdhip64_7.dll failed to load (error %lu), from the system or HIP_PATH. AMD HIP 7 is required "
            "(RX 6000: the AMD HIP SDK 7.2).", hipError);
        return false;
    }
    auto count = reinterpret_cast<int (*)(int *)>(GetProcAddress(hip, "hipGetDeviceCount"));
    auto props = reinterpret_cast<int (*)(void *, int)>(GetProcAddress(hip, "hipGetDevicePropertiesR0600"));
    g.hipSet = reinterpret_cast<HipSetFn>(GetProcAddress(hip, "hipSetDevice"));
    if (count == nullptr || props == nullptr || g.hipSet == nullptr)
    {
        Log("HIP: R0600 API unavailable.");
        g.hipSet = nullptr;
        return false;
    }
    int n = 0;
    if (count(&n) != 0 || n == 0)
    {
        Log("HIP: no devices enumerated.");
        g.hipSet = nullptr;
        return false;
    }
    const auto luid = g.device->GetAdapterLuid();
    for (int i = 0; i < n; ++i)
    {
        alignas(16) std::vector<unsigned char> p(8192, 0);
        if (props(p.data(), i) == 0 && std::memcmp(p.data() + 272, &luid, 8) == 0)
        {
            g.hipDevice = i;
            Log("HIP: adapter %s matches the game's D3D12 device.", reinterpret_cast<char *>(p.data()));
            break;
        }
    }
    if (g.hipDevice < 0 || g.hipSet(g.hipDevice) != 0)
    {
        Log("HIP: no device matches the D3D12 LUID.");
        g.hipSet = nullptr;
        return false;
    }
    return true;
}
