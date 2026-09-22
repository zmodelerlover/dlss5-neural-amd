# Runs one game session with one of the runtime's undocumented environment knobs set, archives the
# two logs, and tabulates the residual measurement across the runs collected so far.
#
# The knobs were found by reading dlssnr_amd_pass1.dll (spike/rocm-custom-kernel/README.md section 9.2).
# The runtime reads each one with getenv, so none of them can be toggled from the overlay: a knob
# costs one relaunch. That is the only reason this script exists.
#
# What it measures. The add-on writes three lines to amd-nr.log on its first successful
# measurement, and 'Measure Residual Again' in the overlay re-arms them:
#
#   measure, network input: mean absolute 0.376325 (466560 samples)
#   measure, residual at 1920x971: mean 0.009285, max 0.106934 (466560 samples)
#   measure, residual detail: local variation of the correction 0.000737 against 0.005573 -- ratio 0.132
#
# The RATIO is the one that answers a question the mean cannot. A grade, a gain, or anything that
# scales the correction moves `mean` and leaves `ratio` roughly where it was. A change in what the
# network actually computes moves where the correction lands, so the ratio moves too. Read both.
#
# THE PROTOCOL, and it is the whole experiment -- get this wrong and the numbers mean nothing:
#   The residual depends on the scene. Comparing two runs of different scenes measures the scenes.
#   So: same save, same spot, same camera, same add-on settings; change ONE env var between runs;
#   and take the baseline again in the same sitting rather than reusing an older log.
#
# Usage:
#   tools\knob_sweep.ps1 -Knob baseline
#   tools\knob_sweep.ps1 -Knob DLSSNR_NOBLEND
#   tools\knob_sweep.ps1 -Knob DLSSNR_STAGES -Value 2
#   tools\knob_sweep.ps1 -Report
#
# In game: let it reach the same spot, then press 'Measure Residual Again' in the add-on overlay
# (or just play past frame 240), then quit. The script waits for the process to exit.

param(
    [string]$Knob = '',
    [string]$Value = '1',
    [string]$Exe = 'D:\pcsx2-v2.8.2-test\pcsx2-qt.exe',
    [string]$OutDir = '',
    [switch]$Report,
    # Unattended mode. -Seconds runs the session for that long and then closes it, instead of
    # waiting for a human to quit. -GameArgs boots straight into a fixed scene, which is what makes
    # the runs comparable: a PCSX2 save state is the same frame every time, which no amount of
    # parking a camera by hand can match.
    [int]$Seconds = 0,
    [string[]]$GameArgs = @(),
    # Seconds to wait before sending the add-on's toggle hotkey (Ctrl+End by default). Leaving the
    # effect off at boot and switching it on afterwards is deliberate: StartOn=1 turns the network
    # on at frame 0, before ReShade has finished compiling AMD_Neural_Feed, and the first attempt
    # at this ran the network at full resolution from boot and took the D3D11 device out.
    [int]$ToggleAfter = 0
)

# Synthesised at the driver level, because the add-on reads the hotkey with GetAsyncKeyState and a
# window message would not register.
Add-Type -Namespace W -Name K -MemberDefinition @'
[DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, System.UIntPtr e);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
'@
function Send-Toggle($proc) {
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { [void][W.K]::SetForegroundWindow($proc.MainWindowHandle) }
    Start-Sleep -Milliseconds 400
    [W.K]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)          # Ctrl down
    [W.K]::keybd_event(0x23, 0, 0, [UIntPtr]::Zero)          # End  down
    Start-Sleep -Milliseconds 80
    [W.K]::keybd_event(0x23, 0, 2, [UIntPtr]::Zero)          # End  up
    [W.K]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)          # Ctrl up
}

$ErrorActionPreference = 'Stop'

$KNOWN = @(
    'DLSSNR_SLOW_PREPOST',  # dedicated k_pre_block/k_post_block instead of the fused k_swin_var<32,true>
    'DLSSNR_NOBLEND',       # forces one post-stage float to 0 (otherwise xmm7)
    'DLSSNR_NOPOSTHIST',    # passes NULL instead of the post-stage history pointer
    'DLSSNR_WBLOG',         # weight-loading logging
    'DLSSNR_STAGES',        # takes a value, not a flag
    'DLSSNR_NO_REPACK'      # boolean, branches inline
)

$gameDir = Split-Path -Parent $Exe
if (-not $OutDir) { $OutDir = Join-Path $gameDir 'amd-nr-knobs' }
$addonLog = Join-Path $gameDir 'amd-nr.log'
$rtLog = Join-Path $gameDir 'dlssnr_on_amd.log'

# --- Parsing ------------------------------------------------------------------------------------
# One archived run -> the numbers, or $null if the run never measured. A run that did not reach the
# measurement is not a zero: it is an absent data point, and the report has to say so rather than
# quietly averaging it in.
function Read-Run($path) {
    $text = Get-Content $path -Raw -ErrorAction SilentlyContinue
    if (-not $text) { return $null }
    $r = [ordered]@{ run = [IO.Path]::GetFileNameWithoutExtension($path) }
    $m = [regex]::Match($text, 'measure, network input: mean absolute ([\d.]+)')
    if ($m.Success) { $r.inputMean = [double]$m.Groups[1].Value }
    $m = [regex]::Match($text, 'measure, residual at (\d+)x(\d+): mean ([\d.]+), max ([\d.]+)')
    if ($m.Success) {
        $r.res = "$($m.Groups[1].Value)x$($m.Groups[2].Value)"
        $r.mean = [double]$m.Groups[3].Value
        $r.max = [double]$m.Groups[4].Value
    }
    $m = [regex]::Match($text, 'residual detail: local variation of the correction ([\d.]+) against ([\d.]+).*?ratio ([\d.]+)')
    if ($m.Success) {
        $r.gRes = [double]$m.Groups[1].Value
        $r.gIn = [double]$m.Groups[2].Value
        $r.ratio = [double]$m.Groups[3].Value
    }
    $r.inert = $text -match 'WARNING: the network is returning its input unchanged'
    if (-not $r.Contains('mean')) { return $null }
    return [pscustomobject]$r
}

function Show-Report {
    if (-not (Test-Path $OutDir)) { throw "No runs collected yet in $OutDir" }
    $runs = Get-ChildItem $OutDir -Filter '*.addon.log' | ForEach-Object { Read-Run $_.FullName } |
            Where-Object { $_ }
    if (-not $runs) { throw "Runs are archived in $OutDir but none of them reached a measurement." }

    $base = $runs | Where-Object { $_.run -like 'baseline*' } | Select-Object -First 1
    # Signed percentage against the baseline. A bare :p1 hides the sign on a rise, and the sign is
    # the whole point of the column, so build it by hand.
    function pct($now, $was) {
        if (-not $was) { return '' }
        $d = (($now / $was) - 1) * 100
        return '{0}{1:N1}%' -f $(if ($d -ge 0) { '+' } else { '' }), $d
    }
    $runs | ForEach-Object {
        $dMean = if ($base -and $_.mean) { pct $_.mean $base.mean } else { '' }
        $dRatio = if ($base -and $_.ratio) { pct $_.ratio $base.ratio } else { '' }
        [pscustomobject]@{
            run       = $_.run -replace '\.addon$', ''
            res       = $_.res
            inputMean = '{0:N6}' -f $_.inputMean
            mean      = '{0:N6}' -f $_.mean
            'd mean'  = $dMean
            ratio     = '{0:N3}' -f $_.ratio
            'd ratio' = $dRatio
            inert     = if ($_.inert) { 'YES' } else { '' }
        }
    } | Format-Table -AutoSize

    if (-not $base) {
        Write-Host "No 'baseline' run archived, so the deltas are blank. Run -Knob baseline in the same scene."
    } else {
        # Single-quoted: a backtick is PowerShell's escape character, so "`ratio`" silently becomes
        # a carriage return plus "atio". It did exactly that the first time this ran.
        Write-Host 'Read it this way: only "mean" moved  -> a gain or a grade, scaled correction.'
        Write-Host '                  "ratio" moved too  -> the network is computing something different.'
        Write-Host "Both are scene-dependent. If the runs were not the same scene, the table is noise."
    }
}

if ($Report) { Show-Report; return }

# --- One run ------------------------------------------------------------------------------------
if (-not $Knob) { throw "Pass -Knob <name|baseline>, or -Report. Known knobs: $($KNOWN -join ', ')" }
if (-not (Test-Path $Exe)) { throw "Executable not found: $Exe" }
if ($Knob -ne 'baseline' -and $KNOWN -notcontains $Knob) {
    Write-Host "Warning: '$Knob' is not one of the six knobs read out of the runtime. Continuing anyway."
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$tag = if ($Knob -eq 'baseline') { 'baseline' } else { "$Knob=$Value" -replace '[^\w=.-]', '_' }

# The runtime's log APPENDS across sessions; the add-on's is truncated each run. So remember where
# the runtime log ended and keep only the tail afterwards, or every archive carries every past run.
$rtWas = if (Test-Path $rtLog) { (Get-Item $rtLog).Length } else { 0 }

if ($Knob -ne 'baseline') {
    Set-Item -Path "env:$Knob" -Value $Value
    Write-Host "set $Knob=$Value"
} else {
    foreach ($k in $KNOWN) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    Write-Host "baseline: all six knobs cleared from this process's environment"
}

Write-Host ""
Write-Host "Launching $Exe $($GameArgs -join ' ')"
$sp = @{ FilePath = $Exe; WorkingDirectory = $gameDir; PassThru = $true }
if ($GameArgs.Count) { $sp.ArgumentList = $GameArgs }
$p = Start-Process @sp

if ($Seconds -gt 0) {
    Write-Host "  unattended: running $Seconds s, then closing. The add-on measures on its own once"
    Write-Host "  it has seen 240 non-black frames, so nothing has to be pressed."
    if ($ToggleAfter -gt 0) {
        if ($p.WaitForExit($ToggleAfter * 1000)) {
            Write-Host "  the game exited before the toggle -- nothing to measure."
        } else {
            Write-Host "  sending Ctrl+End at +$ToggleAfter s to switch the effect on"
            Send-Toggle $p
        }
    }
    if (-not $p.WaitForExit([Math]::Max(1, $Seconds - $ToggleAfter) * 1000)) {
        # CloseMainWindow first: it lets PCSX2 shut down rather than leaving it thinking it crashed.
        # Every Log() call fflushes, so even the hard kill below cannot lose the measurement.
        [void]$p.CloseMainWindow()
        if (-not $p.WaitForExit(15000)) { $p.Kill(); Write-Host "  (had to kill it)" }
    }
} else {
    Write-Host "  In game: reach the SAME spot as the other runs, open the add-on overlay, press"
    Write-Host "  'Measure Residual Again', then quit. Waiting for the process to exit..."
    $p.WaitForExit()
}
Write-Host "exited."

if (-not (Test-Path $addonLog)) { throw "No amd-nr.log in $gameDir -- did the add-on load?" }
Copy-Item $addonLog (Join-Path $OutDir "$tag.addon.log") -Force
if (Test-Path $rtLog) {
    $fs = [IO.File]::Open($rtLog, 'Open', 'Read', 'ReadWrite')
    try {
        $fs.Seek($rtWas, 'Begin') | Out-Null
        $buf = New-Object byte[] ($fs.Length - $rtWas)
        [void]$fs.Read($buf, 0, $buf.Length)
        [IO.File]::WriteAllBytes((Join-Path $OutDir "$tag.runtime.log"), $buf)
    } finally { $fs.Dispose() }
}

$run = Read-Run (Join-Path $OutDir "$tag.addon.log")
if (-not $run) {
    Write-Host ""
    Write-Host "This run never measured -- no 'measure, residual' line in the log."
    Write-Host "Either the session never got past frame 240 with a non-black frame, or the effect"
    Write-Host "was off. Re-run and press 'Measure Residual Again' before quitting."
    return
}
Write-Host ""
Write-Host ("{0}: input {1:N6}, residual mean {2:N6}, ratio {3:N3}{4}" -f `
    $tag, $run.inputMean, $run.mean, $run.ratio, $(if ($run.inert) { '  [WATCHDOG: inert]' } else { '' }))
Write-Host ""
Show-Report
