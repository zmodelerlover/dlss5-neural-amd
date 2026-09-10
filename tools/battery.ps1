# One configuration: set the two inis + optional env var, run, pull the numbers that matter.
param([string]$Label, [string]$NeuralIni = '', [string]$AmdIni = '', [string]$EnvVar = '', [int]$Seconds = 85)

$nfs = 'D:\SteamLibrary\steamapps\common\Need for Speed'
Get-Process NFS16 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 8

function Set-Ini($path, $pairs) {
    if ($pairs -eq '') { return }
    $lines = Get-Content $path
    foreach ($pair in $pairs.Split(',')) {
        $k = $pair.Split('=')[0]
        $lines = $lines | Where-Object { $_ -notmatch ('^' + $k + '=') }
        $lines += $pair
    }
    $lines | Set-Content $path -Encoding ascii
}
Set-Ini "$nfs\dlss5-neural.ini" $NeuralIni
Set-Ini "$nfs\dlssnr_on_amd.ini" $AmdIni

Remove-Item "$nfs\dlss5-neural.log" -ErrorAction SilentlyContinue
Remove-Item "$nfs\dlssnr_on_amd.log" -ErrorAction SilentlyContinue

if ($EnvVar -ne '') {
    $n = $EnvVar.Split('=')[0]; $v = $EnvVar.Split('=')[1]
    Set-Item -Path ("env:" + $n) -Value $v
}
Start-Process "$nfs\NFS16.exe" -WorkingDirectory $nfs
Start-Sleep -Seconds $Seconds
Get-Process NFS16 -ErrorAction SilentlyContinue | Stop-Process -Force
if ($EnvVar -ne '') { Remove-Item ("env:" + $EnvVar.Split('=')[0]) -ErrorAction SilentlyContinue }

$nl  = Get-Content "$nfs\dlss5-neural.log" -ErrorAction SilentlyContinue
$eng = Get-Content "$nfs\dlssnr_on_amd.log" -ErrorAction SilentlyContinue

Write-Output ('===== ' + $Label + ' =====')
$r = @($nl | Select-String -Pattern 'measure, residual')
if ($r.Count -gt 0) { $r | ForEach-Object { Write-Output ('  ' + $_.Line.Trim()) } } else { Write-Output '  measure: nao chegou a medir' }
$mv = @($eng | Select-String -Pattern 'motion: mean')
if ($mv.Count -gt 0) { Write-Output ('  ' + $mv[-1].Line.Trim()) } else { Write-Output '  motor: nenhuma linha de motion' }
$da = @($eng | Select-String -SimpleMatch 'depth autodetect')
if ($da.Count -gt 0) { Write-Output ('  ' + $da[0].Line.Trim()) } else { Write-Output '  motor: depth autodetect NAO disparou' }
$st = @($eng | Select-String -SimpleMatch 'staging ready')
if ($st.Count -gt 0) { Write-Output ('  ' + $st[-1].Line.Trim()) }
$jb = @($eng | Select-String -Pattern 'network job \d+ done')
if ($jb.Count -gt 0) { Write-Output ('  ' + $jb[-1].Line.Trim()) }
$gp = @($nl | Select-String -Pattern 'guide probe')
$gp | Select-Object -Last 2 | ForEach-Object { Write-Output ('  ' + $_.Line.Trim()) }
