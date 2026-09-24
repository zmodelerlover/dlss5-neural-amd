# Gathers the files a release has to publish loose, and writes the SHA256SUMS.txt that pins them.
#
# This exists because of one silent failure: AMD-NR ReShade Installer only offers a version whose
# release publishes BOTH the files a route installs AND a SHA256SUMS.txt naming them. A release
# missing either is skipped without a word -- the version simply never appears in the menu, and
# nothing anywhere says why. Releases.cs is the contract:
#
#   64-bit route : amd-nr.addon64
#   32-bit route : amd-nr.addon32, amd-nr-host64.exe, payload.sha256
#   always       : SHA256SUMS.txt covering all of them
#
# Run build-x86bridge.ps1 first -- it produces every one of these except the sums file.
#
#   .\tools\release-assets.ps1
#   gh release upload v0.5.1 (Get-Content release\upload.txt)
#
# It also prints the payload.json fragment, so the version the installer treats as bundled can be
# moved to this release without hand-copying four hashes.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$release = Join-Path $root 'release'
New-Item -ItemType Directory -Force -Path $release | Out-Null

# Where each one is built, and the name it has to carry in the release.
$wanted = [ordered]@{
    'amd-nr.addon64'    = Join-Path $root 'build\amd-nr.addon64'
    'amd-nr.addon32'    = Join-Path $root 'build-x86bridge\amd-nr.addon32'
    'amd-nr-host64.exe' = Join-Path $root 'build-x86bridge\amd-nr-host64.exe'
    'payload.sha256'          = Join-Path $release 'payload.sha256'
    # Not read by the version menu, which keeps the payload list's own copy, but the release is its
    # mirror: v0.6.6 carried it only because it was added by hand.
    'AMD_Neural_Feed.fx'      = Join-Path $root 'effects\AMD_Neural_Feed.fx'
}

$missing = $wanted.GetEnumerator() | Where-Object { -not (Test-Path -LiteralPath $_.Value) }
if ($missing) {
    throw ("Run .\build-x86bridge.ps1 first; missing: " + (($missing | ForEach-Object { $_.Key }) -join ', '))
}

$rows = @()
$lines = @()
foreach ($entry in $wanted.GetEnumerator()) {
    $file = Get-Item -LiteralPath $entry.Value
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    # Two spaces, the shape sha256sum writes and the installer's parser reads.
    $lines += "$hash  $($entry.Key)"
    $rows += [pscustomobject]@{ Name = $entry.Key; Size = $file.Length; Sha256 = $hash }
}

$sums = Join-Path $release 'SHA256SUMS.txt'
Set-Content -LiteralPath $sums -Value $lines -Encoding ASCII

# The exact list to hand gh, so nothing is forgotten and nothing extra goes up.
$upload = @($wanted.Values) + @($sums)
Set-Content -LiteralPath (Join-Path $release 'upload.txt') -Value $upload -Encoding ASCII

Write-Host ""
Write-Host "release assets, all six:"
$rows | Format-Table -AutoSize
Write-Host "SHA256SUMS.txt -> $sums"
Write-Host "upload list    -> $(Join-Path $release 'upload.txt')"
Write-Host ""
Write-Host "payload.json: addon and bridge, for the installer that ships with this release."
Write-Host "  (version is whatever tag you are publishing; the hashes and sizes are below)"
foreach ($r in $rows) {
    Write-Host ('    {{ "name": "{0}", "size": {1}, "sha256": "{2}" }}' -f $r.Name, $r.Size, $r.Sha256)
}
