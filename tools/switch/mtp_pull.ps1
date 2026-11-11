param([string]$OutDir, [int]$RecentHours = 3)
# Pulls the mvlo logs and recent Atmosphère reports from "1: SD Card" (DBI MTP).

$sh = New-Object -ComObject Shell.Application
$dev = $sh.Namespace(17).Items() | Where-Object { $_.Name -eq 'Périphérique USB MTP' }
if (-not $dev) { "MTP device not found"; exit 1 }
$sd = $dev.GetFolder.Items() | Where-Object { $_.Name -eq '1: SD Card' }

function Get-Sub($folder, [string[]]$names) {
    foreach ($n in $names) {
        $folder = $folder.GetFolder.Items() | Where-Object { $_.Name -eq $n }
        if (-not $folder) { return $null }
    }
    return $folder
}

function Pull($items, $dest) {
    New-Item -ItemType Directory -Force $dest | Out-Null
    $target = $sh.Namespace((Resolve-Path $dest).Path)
    foreach ($it in $items) {
        $target.CopyHere($it, 16)
        $local = Join-Path $dest $it.Name
        $want = [long]$it.ExtendedProperty('System.Size')
        for ($n = 0; $n -lt 120; $n++) {
            if ((Test-Path $local) -and (Get-Item $local).Length -eq $want) { break }
            Start-Sleep -Milliseconds 500
        }
        "{0,10}  {1}" -f $want, $local
    }
}

$mvlo = Get-Sub $sd @('switch', 'mvlo')
$logs = @($mvlo.GetFolder.Items() | Where-Object { -not $_.IsFolder -and $_.Name -match '\.(log|ini)$' })
Pull $logs (Join-Path $OutDir 'mvlo')

$since = (Get-Date).AddHours(-$RecentHours)
foreach ($sub in 'crash_reports', 'fatal_errors', 'erpt_reports') {
    $f = Get-Sub $sd @('atmosphere', $sub)
    if (-not $f) { continue }
    $recent = @($f.GetFolder.Items() | Where-Object { -not $_.IsFolder -and $_.ModifyDate -ge $since })
    "atmosphere/$sub : $($recent.Count) file(s) in the last $RecentHours h"
    if ($recent.Count) { Pull $recent (Join-Path $OutDir $sub) }
}
