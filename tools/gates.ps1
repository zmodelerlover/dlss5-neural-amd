# Every check this repository has, in one pass: the builds, the panel, and each tools/*_check.py.
# -All also builds the diagnostic targets. Prints one line per gate and "FAILED GATES: n" last.
#
#   .\tools\gates.ps1 -All
#
# The checks that compile a function on their own need a MinGW g++ with windows.h: MSYS2's UCRT64
# one when it is at the usual place, else whatever g++ is on PATH (CXX overrides both). CI runs the
# half that needs no GPU and no runtime binary (.github/workflows/build.yml).
param([switch]$All)
$r = Split-Path $PSScriptRoot -Parent
Set-Location $r
$msys = 'C:\msys64\ucrt64\bin'
if (Test-Path "$msys\g++.exe") { $env:PATH = "$msys;$env:PATH" }
if (-not $env:CXX) { $env:CXX = 'g++' }
$fail = 0
function Gate($name, [scriptblock]$run, $okPattern) {
    $o = & $run 2>&1 | Out-String
    $ok = ($LASTEXITCODE -eq 0) -and ($o -match $okPattern)
    if (-not $ok) { $script:fail++ }
    "{0,-26} {1}" -f $name, $(if ($ok) { 'OK' } else { "FAIL`n" + (($o -split "`n") | Select-Object -Last 25 | Out-String) })
}
$targets = @(@('neural', $false), @('framecheck', $true))
if ($All) { $targets += @(@('probe', $false), @('session', $false), @('glinfo', $false), @('glprobe', $true), @('vkprobe', $true), @('vkbridge', $true), @('hostcheck', $true), @('hostcapture', $false)) }
foreach ($t in $targets) {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$r\build.ps1", '-Target', $t[0]); if ($t[1]) { $a += '-Exe' }
    Gate "build $($t[0])" { & powershell @a } 'OK: '
}
Gate 'build-x86bridge' { & powershell -NoProfile -ExecutionPolicy Bypass -File "$r\build-x86bridge.ps1" } 'PASS native x86/x64'
Gate 'ui g++ -Werror' {
    $bad = 0
    foreach ($f in Get-ChildItem "$r\core\ui" -Recurse -Filter *.cpp) {
        & $env:CXX -std=c++20 -Wall -Wextra -Werror -fsyntax-only "-I$r\3rdparty\reshade" $f.FullName
        if ($LASTEXITCODE -ne 0) { $bad++ }
    }
    "ui files failing: $bad"; $global:LASTEXITCODE = [int]($bad -ne 0)
} 'failing: 0'
Gate 'panel_check' {
    $src = @("$r\core\ui\tests\panel_check.cpp") + (Get-ChildItem "$r\core\ui" -Recurse -Filter *.cpp | Where-Object { $_.Directory.Name -ne 'tests' } | ForEach-Object FullName)
    & $env:CXX -std=c++20 -Wall -Wextra -Werror "-I$r\3rdparty\reshade" @src -o "$env:TEMP\panel_check.exe"
    if ($LASTEXITCODE -eq 0) { & "$env:TEMP\panel_check.exe" }
} 'panel_check: PASS'
Gate 'opengl_import (binary)' { python tools/opengl_import_check.py build/amd-nr.addon64 } 'PASS'
Gate 'opengl_import (source)' { python tools/opengl_import_check.py } 'PASS'
Gate 'runtime_offsets' { python tools/runtime_offsets_check.py } 'PASS'
Gate 'compose_check' { python tools/compose_check.py } '.'
Gate 'guide_switch' { python tools/guide_switch_check.py } 'hold'
Gate 'depth_gate_check' { python tools/depth_gate_check.py } 'PASS the depth gate'
Gate 'd3d12_depth_pick' { python tools/d3d12_depth_pick_check.py } 'properties hold'
Gate 'motion_feed' { python tools/motion_feed_check.py } 'PASS'
Gate 'transport_check' { python tools/transport_check.py } 'PASS'
Gate 'unload_check' { python tools/unload_check.py } 'PASS'
Gate 'job_gate_check' { python tools/job_gate_check.py } 'PASS'
Gate 'fence_wait_check' { python tools/fence_wait_check.py } 'PASS the fence wait'
Gate 'watchdog_check' { python tools/watchdog_check.py } 'PASS the watchdog'
Gate 'ini_migrate_check' { python tools/ini_migrate_check.py } 'PASS an ini'
Gate 'raster_pin_check' { python tools/raster_pin_check.py } 'PASS the raster pin'
Gate 'temporal_reset_check' { python tools/temporal_reset_check.py } 'PASS temporal resets'
Gate 'stats_check' { python tools/stats_check.py } 'PASS the stats line'
Gate 'host_check selftest' { python tools/host_check.py --selftest } 'PASS host_check selftest'
Gate 'line_limit' { python tools/line_limit_check.py } 'OK:'
Gate 'test-x86bridge (all 3)' { python core/x86bridge/tests/test-x86bridge.py } 'PASS static boundaries'
Gate 'test-x86bridge-v2' { python core/x86bridge/tests/test-x86bridge-v2.py } 'generic source'
Gate 'test-x86bridge-factory' { python core/x86bridge/tests/test-x86bridge-factory.py } 'PASS factory'
"FAILED GATES: $fail"
