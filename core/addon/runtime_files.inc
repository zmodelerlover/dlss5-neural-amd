// The files the runtime needs beside it before it loads: its own ini, and the copy that imports
// the private D3D12 instead of the system one. Included by neural.cpp where they used to be.

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
