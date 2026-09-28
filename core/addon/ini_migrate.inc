// The one-time moves of an amd-nr.ini written by an older release. Part of the core's unity build,
// included by neural.cpp just before EnsureNeuralIni, which runs both on the 64-bit add-on and on
// the 32-bit bridge's helper. tools/ini_migrate_check.py runs MigrateTemporalDefault.

// v0.6.5 renamed the add-on's files. Somebody upgrading has a dlss5-neural.ini they spent time
// on, and a rename that silently resets every setting to default is a worse first impression than
// any rebrand is worth -- so the old file is carried over once, section header and all, and left
// in place rather than deleted. If both exist the new one wins and nothing is touched.
//
// ponytail: a copy and one string replace. The keys did not change, only the section they sit in
// and the name of the file holding them.
void MigrateLegacyIni()
{
    const auto here = ExeDirectory();
    const auto now = here / L"amd-nr.ini", was = here / L"dlss5-neural.ini";
    std::error_code ec;
    if (std::filesystem::exists(now, ec) || !std::filesystem::exists(was, ec))
        return;

    std::ifstream in(was, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (text.empty())
        return;
    if (const size_t at = text.find("[dlss5]"); at != std::string::npos)
        text.replace(at, 7, "[amd-nr]");

    std::ofstream out(now, std::ios::binary);
    if (!out)
        return;
    out << text;
    out.close();
    Log("carried dlss5-neural.ini over to amd-nr.ini; the old file is left where it was.");
}

// v0.7.0 made Temporal On (2) the default, with Fixed seed and Output smoothing. Every ini written
// before it holds Temporal=0, the default then, and no FixedSeed key, since no release before it
// had one; and nothing tells a 0 somebody chose from the 0 that was simply written. The decision
// (27/09/2026) was to move those once: Temporal to 2, and the other three keys written as they
// already read, so the file says what runs and a FixedSeed key marks it as done. From then on a
// Temporal=0 in the file is the person's and stays. An ini that says anything but 0 is left alone.
void MigrateTemporalDefault(const std::filesystem::path &ini)
{
    wchar_t value[16] {};
    const auto file = ini.c_str();
    if (GetPrivateProfileStringW(L"amd-nr", L"FixedSeed", L"", value, 16, file) > 0)
        return;
    GetPrivateProfileStringW(L"amd-nr", L"Temporal", L"", value, 16, file);
    if (std::wcscmp(value, L"0") != 0)
        return;
    WritePrivateProfileStringW(L"amd-nr", L"Temporal", L"2", file);
    WritePrivateProfileStringW(L"amd-nr", L"FixedSeed", L"1", file);
    WritePrivateProfileStringW(L"amd-nr", L"OutputSmooth", L"0.8", file);
    WritePrivateProfileStringW(L"amd-nr", L"OutputSmoothLimit", L"10", file);
    Log("amd-nr.ini is from before v0.7.0 and had Temporal=0, the default then: moved once to "
        "Temporal=2, the default now, with FixedSeed=1, OutputSmooth=0.8 and OutputSmoothLimit=10 "
        "written as they already read. A Temporal=0 set from here on stays.");
}
