# Launch NFS and stop the instant the answer is known, either way.
#   PASS         - a resize completed with no failures while frames were being processed
#   ERROR-DIALOG - the game's own DirectX box
#   NO-FRAMES    - the add-on came up but never processed a frame
#   PROCESS-EXITED / TIMEOUT
param([int]$MaxSeconds = 60, [string]$Ini = '', [int]$FrameGrace = 20)

$nfs = 'D:\SteamLibrary\steamapps\common\Need for Speed'
Get-Process NFS16 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 8

if ($Ini -ne '') {
    $lines = Get-Content "$nfs\dlss5-neural.ini"
    foreach ($pair in $Ini.Split(',')) {
        $k = $pair.Split('=')[0]
        $lines = $lines | Where-Object { $_ -notmatch ('^' + $k + '=') }
        $lines += $pair
    }
    $lines | Set-Content "$nfs\dlss5-neural.ini" -Encoding ascii
}
Remove-Item "$nfs\dlss5-neural.log" -ErrorAction SilentlyContinue
Remove-Item "$nfs\ReShade.log" -ErrorAction SilentlyContinue

$sw = [Diagnostics.Stopwatch]::StartNew()
Start-Process "$nfs\NFS16.exe" -WorkingDirectory $nfs

# NFS16.exe relaunches itself through the store front end, so the first process going away is
# normal. Wait for ReShade to come up before treating anything as a verdict.
$ready = $false
while ($sw.Elapsed.TotalSeconds -lt 60) {
    Start-Sleep -Milliseconds 400
    if ((Test-Path "$nfs\ReShade.log") -and (Get-Process NFS16 -ErrorAction SilentlyContinue)) { $ready = $true; break }
}
if (-not $ready) { Write-Output 'verdict   : NEVER-STARTED'; return }

$readyAt = $sw.Elapsed.TotalSeconds
$verdict = 'TIMEOUT'
while ($sw.Elapsed.TotalSeconds -lt $MaxSeconds) {
    Start-Sleep -Milliseconds 400
    $dlg = Get-Process -ErrorAction SilentlyContinue |
           Where-Object { $_.MainWindowTitle -match 'DirectX' }
    if ($dlg) { $verdict = 'ERROR-DIALOG'; break }
    if (-not (Get-Process NFS16 -ErrorAction SilentlyContinue)) { $verdict = 'PROCESS-EXITED'; break }

    $log = Get-Content "$nfs\ReShade.log" -ErrorAction SilentlyContinue
    $nl  = Get-Content "$nfs\dlss5-neural.log" -ErrorAction SilentlyContinue
    $tried  = @($log | Select-String -SimpleMatch 'ResizeBuffers(').Count
    $failed = @($log | Select-String -SimpleMatch 'ResizeBuffers failed').Count
    $frames = @($nl | Select-String -Pattern 'frame \d+ processed').Count
    $engineUp = @($nl | Select-String -SimpleMatch 'engine ready').Count

    if ($failed -gt 0) { $verdict = 'RESIZE-FAILED'; break }
    # The whole question: a resize survived while the add-on was actually working.
    if ($tried -ge 1 -and $frames -ge 2) { $verdict = 'PASS'; break }
    # Came up but never rendered: no point waiting out the clock.
    if ($engineUp -ge 1 -and $frames -eq 0 -and ($sw.Elapsed.TotalSeconds - $readyAt) -gt $FrameGrace) {
        $verdict = 'NO-FRAMES'; break
    }
}
$elapsed = [math]::Round($sw.Elapsed.TotalSeconds)
Get-Process NFS16 -ErrorAction SilentlyContinue | Stop-Process -Force

$log = Get-Content "$nfs\ReShade.log" -ErrorAction SilentlyContinue
$nl  = Get-Content "$nfs\dlss5-neural.log" -ErrorAction SilentlyContinue
$tried  = @($log | Select-String -SimpleMatch 'ResizeBuffers(').Count
$failed = @($log | Select-String -SimpleMatch 'ResizeBuffers failed').Count
$hooks  = @($log | Select-String -Pattern 'delayed hooks.*d3d12').Count
$fr     = @($nl | Select-String -Pattern 'frame \d+ processed')
Write-Output ('verdict   : ' + $verdict + '  after ' + $elapsed + 's')
Write-Output ('resizes   : tried ' + $tried + ', failed ' + $failed + '   d3d12 hooks: ' + $hooks)
if ($fr.Count -gt 0) { Write-Output ('frames    : ' + $fr[-1].Line) } else { Write-Output 'frames    : none' }
$nl | Select-String -Pattern 'D3D12 loaded|bridge:|engine ready|guide |depth:|motion:' |
      Select-Object -Last 4 | ForEach-Object { Write-Output ('  ' + $_.Line) }
