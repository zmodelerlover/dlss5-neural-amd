# Builds the add-ons with the MSVC cl.exe directly, no Visual Studio project.
# Produces a .addon64, which ReShade loads by extension.
#
# The toolchain is discovered: Visual Studio through vswhere, the Windows SDK through the
# registry. Override either with -VsPath / -SdkPath if discovery picks the wrong one.

param(
    [string]$Target = 'neural',
    [string]$VsPath = '',
    [string]$SdkPath = '',
    [string]$SdkVersion = '',
    # Targets are ReShade add-ons by default. -Exe builds a console program instead, for the
    # harnesses that answer a question without needing a host to inject into.
    [switch]$Exe
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# --- Visual Studio -----------------------------------------------------------------------
if (-not $VsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Install Visual Studio with the C++ workload, or pass -VsPath."
    }
    $VsPath = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1
}

# vswhere does not see every install -- Insiders/preview builds in particular. Fall back to
# scanning the standard locations for the tools themselves.
if (-not $VsPath -or -not (Test-Path (Join-Path $VsPath 'VC\Tools\MSVC'))) {
    $VsPath = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) |
        Where-Object { $_ } |
        ForEach-Object { Join-Path $_ 'Microsoft Visual Studio' } |
        Where-Object { Test-Path $_ } |
        ForEach-Object { Get-ChildItem $_ -Directory -Recurse -Depth 1 -ErrorAction SilentlyContinue } |
        Where-Object { Test-Path (Join-Path $_.FullName 'VC\Tools\MSVC') } |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $VsPath) {
    throw "No Visual Studio install with the C++ tools was found. Pass -VsPath."
}

$msvcRoot = Join-Path $VsPath 'VC\Tools\MSVC'
if (-not (Test-Path $msvcRoot)) { throw "MSVC tools not found under $msvcRoot" }
$msvc = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $cl)) { throw "cl.exe not found at $cl" }

# --- Windows SDK -------------------------------------------------------------------------
if (-not $SdkPath) {
    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0',
                     'HKLM:\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v10.0') {
        try { $SdkPath = (Get-ItemProperty $key -ErrorAction Stop).InstallationFolder } catch {}
        if ($SdkPath) { break }
    }
}
if (-not $SdkPath -or -not (Test-Path $SdkPath)) {
    throw "Windows 10/11 SDK not found in the registry. Pass -SdkPath."
}

if (-not $SdkVersion) {
    # Newest version that actually has the headers we need.
    $SdkVersion = Get-ChildItem (Join-Path $SdkPath 'Include') -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } |
        Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
}
if (-not $SdkVersion) { throw "No usable SDK version found under $SdkPath\Include. Pass -SdkVersion." }

Write-Host "MSVC $($msvc.Name)"
Write-Host "SDK  $SdkVersion  ($SdkPath)"

# INCLUDE and LIB are environment variables, so spaces in these paths are fine.
$env:INCLUDE = @(
    (Join-Path $msvc.FullName 'include')
    (Join-Path $SdkPath "Include\$SdkVersion\ucrt")
    (Join-Path $SdkPath "Include\$SdkVersion\um")
    (Join-Path $SdkPath "Include\$SdkVersion\shared")
    (Join-Path $SdkPath "Include\$SdkVersion\winrt")
    (Join-Path $root 'external\reshade')
) -join ';'

$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\x64')
    (Join-Path $SdkPath "Lib\$SdkVersion\ucrt\x64")
    (Join-Path $SdkPath "Lib\$SdkVersion\um\x64")
) -join ';'

# --- compile -----------------------------------------------------------------------------
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$src = Join-Path $root "src\$Target\$Target.cpp"
if (-not (Test-Path $src)) { throw "source not found: $src" }

$dll = Join-Path $out $(if ($Exe) { "dlss5-$Target.exe" } else { "dlss5-$Target.addon64" })

Push-Location $out
try {
    # NOMINMAX: without it the max/min macros in windows.h swallow std::max/std::min.
    & $cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MD /W3 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS `
          /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
          /Fo"$out\" $(if (-not $Exe) { '/LD' }) $src /link $(if (-not $Exe) { '/DLL' }) /OUT:"$dll" `
          user32.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib bcrypt.lib
    if ($LASTEXITCODE -ne 0) { throw "compilation failed ($LASTEXITCODE)" }
} finally { Pop-Location }

Write-Host ""
Write-Host "OK: $dll"
Get-Item $dll | Select-Object Name, Length, LastWriteTime
