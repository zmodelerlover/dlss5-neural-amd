# The compute shaders live as string literals in core\shaders\*.h and are compiled by D3DCompile at
# runtime, so build.ps1 succeeding says nothing about them -- a syntax error only shows up as
# "shader X failed to compile" in amd-nr.log, in a game, after a launch. Pull them out and
# run fxc over them instead.
#
#   powershell -File tools\check_shaders.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$sdk = $null
foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0',
                 'HKLM:\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v10.0') {
    try { $sdk = (Get-ItemProperty $key -ErrorAction Stop).InstallationFolder } catch {}
    if ($sdk) { break }
}
$fxc = $null
if ($sdk -and (Test-Path (Join-Path $sdk 'bin'))) {
    $fxc = Get-ChildItem (Join-Path $sdk 'bin') -Filter fxc.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' } |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $fxc) { throw 'fxc.exe not found. Install the Windows 10/11 SDK.' }

# They moved out of neural.cpp into these headers, and a check still reading neural.cpp found none
# and said every shader compiled.
$src = (Get-ChildItem (Join-Path $root 'core\shaders') -Filter *.h | ForEach-Object { Get-Content $_.FullName -Raw }) -join "`n"
$tmp = Join-Path $env:TEMP ('amd-nr-shaders-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$failed = 0
$shaders = [regex]::Matches($src, 'constexpr char (k\w+)\[\] = R"\((?<body>.*?)\)";', 'Singleline')
if ($shaders.Count -eq 0) { throw 'No shaders found under core\shaders: this check would pass having read nothing.' }
foreach ($m in $shaders) {
    $name = $m.Groups[1].Value
    $file = Join-Path $tmp "$name.hlsl"
    Set-Content -Path $file -Value $m.Groups['body'].Value -Encoding ascii
    $out = & $fxc /nologo /T cs_5_0 /E main /O3 /Fo NUL $file
    if ($LASTEXITCODE -eq 0) {
        '{0,-18} OK' -f $name
    } else {
        '{0,-18} FAIL' -f $name
        $out
        $failed++
    }
}
Remove-Item $tmp -Recurse -Force

if ($failed -ne 0) { throw "$failed shader(s) failed to compile." }
Write-Host ''
Write-Host 'All shaders compile.'
