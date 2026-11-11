param([string]$LocalDir, [string]$StageRoot)
# Copies to "1: SD Card/switch/<name>" (DBI MTP) only the files of $LocalDir
# that are missing there or have the wrong size, then waits until all match.
# FolderItem.Size is 0 over MTP, so sizes come from the System.Size property.

$name = Split-Path $LocalDir -Leaf
$expected = @{}
Get-ChildItem -Recurse -File $LocalDir | ForEach-Object {
    $expected[$_.FullName.Substring($LocalDir.Length + 1).Replace('\', '/')] = $_.Length
}

function Get-SwitchFolder {
    $sh = New-Object -ComObject Shell.Application
    $dev = $sh.Namespace(17).Items() | Where-Object { $_.Name -eq 'Périphérique USB MTP' }
    if (-not $dev) { return $null }
    $sd = $dev.GetFolder.Items() | Where-Object { $_.Name -eq '1: SD Card' }
    return $sd.GetFolder.Items() | Where-Object { $_.Name -eq 'switch' }
}

function Get-Remote($folder, $prefix, $acc) {
    foreach ($it in $folder.Items()) {
        $rel = if ($prefix) { "$prefix/$($it.Name)" } else { $it.Name }
        if ($it.IsFolder) { Get-Remote $it.GetFolder $rel $acc }
        else { $acc[$rel] = [long]$it.ExtendedProperty('System.Size') }
    }
}

function Get-Missing {
    $switch = Get-SwitchFolder
    if (-not $switch) { return $null }
    $dst = $switch.GetFolder.Items() | Where-Object { $_.Name -eq $name }
    $remote = @{}
    if ($dst) { Get-Remote $dst.GetFolder '' $remote }
    # leading comma: keep an empty array from collapsing to $null
    return , @($expected.Keys | Where-Object { -not ($remote.ContainsKey($_) -and $remote[$_] -eq $expected[$_]) })
}

function Wait-Missing {
    # The device drops out now and then: give it 3 min to come back.
    for ($n = 0; $n -lt 36; $n++) {
        try { $m = Get-Missing } catch { $m = $null }
        if ($null -ne $m) { return , $m }
        Start-Sleep -Seconds 5
    }
    return $null
}

for ($round = 1; $round -le 10; $round++) {
    $missing = Wait-Missing
    if ($null -eq $missing) { "$(Get-Date -Format T)  MTP device gone for 3 min"; exit 1 }
    "$(Get-Date -Format T)  round $round : $($missing.Count) of $($expected.Count) files to send"
    if ($missing.Count -eq 0) { "DONE"; exit 0 }

    $stage = Join-Path $StageRoot $name
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    foreach ($rel in $missing) {
        $to = Join-Path $stage $rel
        New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
        Copy-Item (Join-Path $LocalDir $rel) $to
    }

    # 16 = Yes to All (merge folders, replace truncated files)
    try { (Get-SwitchFolder).GetFolder.CopyHere($stage, 16) } catch { "CopyHere failed: $_"; continue }

    # The shell copy runs inside this process: stay alive until it is complete.
    $last = -1; $lastChange = Get-Date
    while ($true) {
        Start-Sleep -Seconds 30
        try { $missing = Get-Missing } catch { $missing = $null }
        if ($null -eq $missing) { "$(Get-Date -Format T)  device disconnected"; break }
        "$(Get-Date -Format T)  $($expected.Count - $missing.Count)/$($expected.Count) OK"
        if ($missing.Count -eq 0) { "DONE"; exit 0 }
        if ($missing.Count -ne $last) { $last = $missing.Count; $lastChange = Get-Date }
        elseif (((Get-Date) - $lastChange).TotalMinutes -ge 5) {
            "STALLED 5 min, still missing:"; $missing | Select-Object -First 20; exit 1
        }
    }
}
"gave up after 10 rounds"
exit 1
