# Builds the installer. Same toolchain discovery as the add-on's build.ps1, because cargo on the
# msvc target shells out to link.exe and finds it on PATH -- which a plain PowerShell does not have.
#
#   ./build.ps1              release build
#   ./build.ps1 -Debug       faster build, no optimisation
#
# The add-on is embedded into the executable, so ../build/dlss5-neural.addon64 has to exist first:
#
#   cd ..; ./build.ps1 -Target neural

param(
    [switch]$Debug,
    [string]$VsPath = ''
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$addon = Join-Path $root '..\build\dlss5-neural.addon64'
if (-not (Test-Path $addon)) {
    throw "The add-on is not built: $addon is missing. Run ./build.ps1 -Target neural in the repository root first."
}

# --- Visual Studio, the same two ways the add-on's build script looks -------------------------
if (-not $VsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $VsPath = & $vswhere -latest -prerelease -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
            Select-Object -First 1
    }
}
# vswhere does not see every install -- Insiders and preview builds in particular.
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
if (-not $VsPath) { throw 'No Visual Studio install with the C++ tools was found. Pass -VsPath.' }

$msvc = Get-ChildItem (Join-Path $VsPath 'VC\Tools\MSVC') -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
$msvcBin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
if (-not (Test-Path (Join-Path $msvcBin 'link.exe'))) { throw "link.exe not found in $msvcBin" }

# --- Windows SDK, for the import libraries the linker needs -----------------------------------
$sdkRoot = $null
foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0',
                 'HKLM:\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v10.0') {
    try { $sdkRoot = (Get-ItemProperty $key -ErrorAction Stop).InstallationFolder } catch {}
    if ($sdkRoot) { break }
}
if (-not $sdkRoot) { throw 'Windows 10/11 SDK not found in the registry.' }
$sdkVer = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'um\windows.h') } |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name

Write-Host "MSVC $($msvc.Name)"
Write-Host "SDK  $sdkVer  ($sdkRoot)"

$env:PATH = "$msvcBin;$env:PATH"
$env:LIB = @(
    (Join-Path $msvc.FullName 'lib\x64')
    (Join-Path $sdkRoot "Lib\$sdkVer\ucrt\x64")
    (Join-Path $sdkRoot "Lib\$sdkVer\um\x64")
) -join ';'

Push-Location $root
try {
    # cargo writes its progress to stderr, which under ErrorActionPreference=Stop is treated as a
    # failed command even when it exits 0. The exit code is the thing to trust here.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    if ($Debug) { cargo build } else { cargo build --release }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) { throw "cargo failed ($code)" }
} finally { Pop-Location }

$exe = Join-Path $root ("target\" + $(if ($Debug) { 'debug' } else { 'release' }) + "\dlss5-installer.exe")
Write-Host ''
Write-Host "OK: $exe"
Get-Item $exe | Select-Object Name, Length, LastWriteTime
