# Resolve a Steam game directory without hardcoding anyone's library layout.
# Dot-source this to get Find-SteamGame:
#
#     . (Join-Path $PSScriptRoot 'find-steam-game.ps1')
#     $game = Find-SteamGame -Name 'Need for Speed' -Explicit $GamePath -EnvName 'AMDNR_NFS_PATH'
#
# Order of precedence: an explicit path wins, then the environment variable, then whatever Steam
# itself reports. Steam records its own install path in the registry, and steamapps\libraryfolders.vdf
# lists every library it knows about, including ones on other drives. Failing all three is an error
# with the ways to fix it, never a silent fallback to someone else's drive letter.

function Find-SteamGame {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$Explicit = '',
        [string]$EnvName = ''
    )

    if ($Explicit -ne '') {
        if (-not (Test-Path -LiteralPath $Explicit)) { throw "Game directory not found: $Explicit" }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }

    if ($EnvName -ne '') {
        $fromEnv = [Environment]::GetEnvironmentVariable($EnvName)
        if ($fromEnv) {
            if (-not (Test-Path -LiteralPath $fromEnv)) { throw "$EnvName points at a directory that does not exist: $fromEnv" }
            return (Resolve-Path -LiteralPath $fromEnv).Path
        }
    }

    $libraries = @()
    try {
        $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
    } catch { $steam = $null }
    if ($steam) {
        $libraries += $steam
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            # Library paths are quoted and backslash-escaped inside the VDF.
            $libraries += Get-Content -LiteralPath $vdf | ForEach-Object {
                if ($_ -match '"path"\s+"(.+?)"') { $Matches[1] -replace '\\\\', '\' }
            }
        }
    }

    foreach ($library in ($libraries | Select-Object -Unique)) {
        $candidate = Join-Path $library ('steamapps\common\' + $Name)
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }

    $hint = if ($EnvName -ne '') { " or set $EnvName" } else { '' }
    throw "Could not find '$Name' in any Steam library. Pass -GamePath$hint."
}
