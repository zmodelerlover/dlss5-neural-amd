# Build and validate the experimental x86 bridge together with the current addon64 checkout.
param([string]$VsPath='', [string]$SdkPath='', [string]$SdkVersion='')
$ErrorActionPreference='Stop'
if([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT){throw 'Native Windows MSVC required; no substitute ABI/compiler.'}
$root=$PSScriptRoot
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


$out=Join-Path $root 'build-x86bridge'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:INCLUDE=@((Join-Path $msvc.FullName 'include'),(Join-Path $SdkPath "Include\$SdkVersion\ucrt"),(Join-Path $SdkPath "Include\$SdkVersion\um"),(Join-Path $SdkPath "Include\$SdkVersion\shared"),(Join-Path $SdkPath "Include\$SdkVersion\winrt"),(Join-Path $root '3rdparty\reshade')) -join ';'
$flags=@('/nologo','/utf-8','/std:c++20','/EHsc','/O2','/MT','/W3','/DUNICODE','/D_UNICODE','/D_CRT_SECURE_NO_WARNINGS','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN')
function PE([string]$p,[int]$machine){
    $b=[IO.File]::ReadAllBytes($p);$e=[BitConverter]::ToInt32($b,0x3c)
    if([BitConverter]::ToUInt32($b,$e) -ne 0x4550 -or [BitConverter]::ToUInt16($b,$e+4) -ne $machine){throw "Wrong PE machine: $p"}
}
Push-Location $out
try {
    foreach($arch in @('x86','x64')){
        $cl=Join-Path $msvc.FullName "bin\Hostx64\$arch\cl.exe"
        if(!(Test-Path -LiteralPath $cl)){throw "Missing native compiler: $cl"}
        $env:LIB=@((Join-Path $msvc.FullName "lib\$arch"),(Join-Path $SdkPath "Lib\$SdkVersion\ucrt\$arch"),(Join-Path $SdkPath "Lib\$SdkVersion\um\$arch")) -join ';'
        $proto=Join-Path $out "protocol-$arch.exe"
        & $cl @flags (Join-Path $root 'src/x86bridge/protocol_test.cpp') "/Fo$out\protocol-$arch.obj" /link "/OUT:$proto" 2>&1 | Tee-Object -FilePath (Join-Path $out "protocol-build-$arch.log")
        if($LASTEXITCODE -ne 0){throw "Protocol compile failed: $arch"}
        & $proto | Tee-Object -FilePath (Join-Path $out "protocol-test-$arch.log")
        if($LASTEXITCODE -ne 0){throw "Protocol test failed: $arch"}
        $ioTest=Join-Path $out "io-test-$arch.exe"
        & $cl @flags (Join-Path $root 'src/x86bridge/io_test.cpp') "/Fo$out\io-test-$arch.obj" /link "/OUT:$ioTest" 2>&1 | Tee-Object -FilePath (Join-Path $out "io-test-build-$arch.log")
        if($LASTEXITCODE -ne 0){throw "IPC test compile failed: $arch"}
        & $ioTest | Tee-Object -FilePath (Join-Path $out "io-test-$arch.log")
        if($LASTEXITCODE -ne 0){throw "IPC test failed: $arch"}
        $captureTest=Join-Path $out "capture-test-$arch.exe"
        & $cl @flags (Join-Path $root 'src/x86bridge/capture_test.cpp') "/Fo$out\capture-test-$arch.obj" /link "/OUT:$captureTest" user32.lib 2>&1 | Tee-Object -FilePath (Join-Path $out "capture-test-build-$arch.log")
        if($LASTEXITCODE -ne 0){throw "Hotkey capture test compile failed: $arch"}
        & $captureTest | Tee-Object -FilePath (Join-Path $out "capture-test-$arch.log")
        if($LASTEXITCODE -ne 0){throw "Hotkey capture test failed: $arch"}
        # The shared panel in src/ui is one object per file, and the x86 and x64 builds of the same
        # file share a name, so each architecture gets its own object folder.
        $obj=Join-Path $out "obj-$arch";New-Item -ItemType Directory -Force -Path $obj | Out-Null
        $ui=Get-ChildItem (Join-Path $root 'src/ui') -Recurse -Filter *.cpp | ForEach-Object FullName
        if($arch -eq 'x86'){
            $binary=Join-Path $out 'amd-nr.addon32'
            & $cl @flags /LD (Join-Path $root 'src/x86bridge/frontend32.cpp') (Join-Path $root 'src/x86bridge/panel32.cpp') $ui "/Fo$obj\" /link /DLL "/OUT:$binary" user32.lib d3d9.lib d3d11.lib dxgi.lib d3dcompiler.lib shell32.lib ole32.lib 2>&1 | Tee-Object -FilePath (Join-Path $out 'build-x86.log')
        }else{
            $binary=Join-Path $out 'amd-nr-host64.exe'
            & $cl @flags (Join-Path $root 'src/x86bridge/host64.cpp') $ui "/Fo$obj\" /link "/OUT:$binary" user32.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib bcrypt.lib shell32.lib ole32.lib 2>&1 | Tee-Object -FilePath (Join-Path $out 'build-x64.log')
        }
        if($LASTEXITCODE -ne 0){throw "Compile failed: $arch"}
        PE $binary $(if($arch -eq 'x86'){0x14c}else{0x8664})
        $dumpbin=Join-Path (Split-Path $cl -Parent) 'dumpbin.exe'
        $imports=& $dumpbin /imports $binary 2>&1
        if($LASTEXITCODE -ne 0){throw 'dumpbin failed'}
        $imports | Set-Content -LiteralPath (Join-Path $out "imports-$arch.txt")
        $text=$imports -join "`n"
        if($text -match '(?i)(VCRUNTIME|MSVCP|libgcc|libstdc\+\+)'){throw "Unexpected C++ DLL dependency: $arch"}
        if($arch -eq 'x86' -and $text -match '(?i)(d3d12\.dll|amdhip64_7\.dll|dlssnr_amd_pass1\.dll)'){throw 'Forbidden frontend import'}
    }
    # The bridge payloads are staged into release/ for whatever installs them: AMD-NR ReShade
    # Installer pins these three by hash and fetches them from the release, and package-release.ps1
    # puts the same three in the by-hand archive. Two earlier installers were built from this repo
    # and both are retired.
    $release=Join-Path $root 'release'
    New-Item -ItemType Directory -Force -Path (Join-Path $release 'files') | Out-Null
    foreach($name in @('amd-nr.addon32','amd-nr-host64.exe')){
        Copy-Item -LiteralPath (Join-Path $out $name) -Destination (Join-Path $release 'files') -Force
    }
    @('amd-nr.addon32','amd-nr-host64.exe') | ForEach-Object {"$((Get-FileHash -LiteralPath (Join-Path $release "files/$_") -Algorithm SHA256).Hash.ToLowerInvariant())  $_"} | Set-Content -Encoding ASCII -LiteralPath (Join-Path $release 'payload.sha256')
    # Build the current integrated addon64 from the same checkout. Git already records whether
    # this feature changed existing sources; byte hashes tied to an older checkout are brittle
    # across rebases and Windows line-ending conversion.
    & (Join-Path $root 'build.ps1') -Target neural -VsPath $VsPath -SdkPath $SdkPath -SdkVersion $SdkVersion 2>&1 | Tee-Object -FilePath (Join-Path $out 'addon64-build.log')
    if($LASTEXITCODE -ne 0){throw 'Integrated addon64 build failed'}
    $original=Join-Path $root 'build/amd-nr.addon64';if(!(Test-Path -LiteralPath $original)){throw 'Integrated addon64 output missing'}
    PE $original 0x8664
    Get-ChildItem -LiteralPath $out -File | Where-Object {$_.Name -ne 'SHA256SUMS.txt'} | Sort-Object Name | ForEach-Object {"$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)  $($_.Name)"} | Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt')
    Write-Host 'PASS native x86/x64 builds, protocol, IPC and hotkey-capture tests, PE, imports and integrated addon64 build. GPU/game tests still require live validation.'
    Write-Host "Outputs: $out"
} finally {Pop-Location}
