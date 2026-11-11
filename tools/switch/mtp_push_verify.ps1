param([string]$LocalDir, [string]$VerifyDir, [int]$Minutes = 15, [switch]$NoVerify)
# Copies $LocalDir into "1: SD Card/switch/" (DBI MTP), replacing existing files,
# then reads every file back and compares SHA-256 (sizes alone can't tell two
# builds of the .nro apart). -NoVerify skips the read-back (~15 s a file over
# MTP): check with ftp_push_verify.py --check instead.

$sh = New-Object -ComObject Shell.Application

# CopyHere's "Yes to All" flag doesn't reach MTP devices: the shell still asks
# "merge folder mvlo?" / "replace file?" in a window of this process, often
# behind a fullscreen app, and the copy waits there forever (2026-11-03).
# Answer those for it: tick "do this for all", then Yes / Replace.
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class MtpConfirm {
  delegate bool P(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(P p, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr w, P p, IntPtr l);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  static string Txt(IntPtr h) { var t = new StringBuilder(512); GetWindowText(h, t, 512); return t.ToString().Replace("&", ""); }
  static string Cls(IntPtr h) { var c = new StringBuilder(64); GetClassName(h, c, 64); return c.ToString(); }
  public static int Answer(uint pid) {
    int n = 0;
    EnumWindows((w, l) => {
      uint p; GetWindowThreadProcessId(w, out p);
      if (p != pid || !IsWindowVisible(w) || Cls(w) != "#32770") return true;
      IntPtr yes = IntPtr.Zero, all = IntPtr.Zero;
      EnumChildWindows(w, (h, l2) => {
        if (Cls(h) == "Button" && IsWindowVisible(h)) {
          string t = Txt(h);
          if (t.StartsWith("Faire ceci") || t.StartsWith("Do this")) all = h;
          if (t == "Oui" || t == "Yes" || t.StartsWith("Remplacer") || t.StartsWith("Replace")) yes = h;
        }
        return true; }, IntPtr.Zero);
      if (yes == IntPtr.Zero) return true;            // the progress window
      if (all != IntPtr.Zero && SendMessage(all, 0xF0, IntPtr.Zero, IntPtr.Zero) == IntPtr.Zero)
        SendMessage(all, 0xF5, IntPtr.Zero, IntPtr.Zero);   // BM_GETCHECK, BM_CLICK
      PostMessage(yes, 0xF5, IntPtr.Zero, IntPtr.Zero);
      n++;
      return true; }, IntPtr.Zero);
    return n; }
  // The shell's copy window ("Copie de ...") is up while a copy runs.
  public static bool Busy(uint pid) {
    bool busy = false;
    EnumWindows((w, l) => {
      uint p; GetWindowThreadProcessId(w, out p);
      if (p == pid && IsWindowVisible(w) && Cls(w) == "#32770") busy = true;
      return !busy; }, IntPtr.Zero);
    return busy; } }
"@
function Confirm-Copy { [void][MtpConfirm]::Answer([uint32]$PID) }

# While a big file is being written, the device can list nothing for a while:
# every lookup returns $null then, and callers just wait and ask again.
function Get-SwitchFolder {
    try {
        $dev = $sh.Namespace(17).Items() | Where-Object { $_.Name -eq 'Périphérique USB MTP' }
        if (-not $dev) { return $null }
        $sd = $dev.GetFolder.Items() | Where-Object { $_.Name -eq '1: SD Card' }
        if (-not $sd) { return $null }
        return $sd.GetFolder.Items() | Where-Object { $_.Name -eq 'switch' }
    } catch { return $null }
}

function Get-RemoteItem($root, [string]$rel) {
    $it = $root
    foreach ($p in $rel.Split('/')) {
        if (-not $it) { return $null }
        try { $it = $it.GetFolder.Items() | Where-Object { $_.Name -eq $p } } catch { return $null }
        if (-not $it) { return $null }
    }
    return $it
}

# Remote sizes of $files, one listing per folder: listing Data/ (~1600 entries)
# over MTP takes seconds, so looking each file up on its own took tens of
# minutes per pass (2026-11-03).
function Get-RemoteSizes($root) {
    $sizes = @{}
    foreach ($dir in ($files | ForEach-Object { (Split-Path $_.Rel).Replace('\', '/') } | Sort-Object -Unique)) {
        $folder = if ($dir) { Get-RemoteItem $root $dir } else { $root }
        if (-not $folder) { continue }
        try { $items = @($folder.GetFolder.Items()) } catch { continue }
        $prefix = if ($dir) { "$dir/" } else { '' }
        foreach ($it in $items) { $sizes[$prefix + $it.Name] = [long]$it.ExtendedProperty('System.Size') }
    }
    return $sizes
}

$switch = Get-SwitchFolder
if (-not $switch) { "MTP device not found"; exit 1 }
$name = Split-Path $LocalDir -Leaf
$files = Get-ChildItem -Recurse -File $LocalDir | ForEach-Object {
    [pscustomobject]@{
        Rel  = $_.FullName.Substring($LocalDir.Length + 1).Replace('\', '/')
        Size = $_.Length
        Hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
    }
}
"$($files.Count) files to send"

# 16 = Yes to All (merge into the existing folder, replace files)
$switch.GetFolder.CopyHere($LocalDir, 16)

# Wait until every file is there with the right size (max $Minutes min). The copy
# runs inside this process: never stop it early, and never tell the user the
# console can be unplugged before "DONE".
$deadline = (Get-Date).AddMinutes($Minutes)
do {
    for ($t = 0; $t -lt 6; $t++) { Start-Sleep -Milliseconds 500; Confirm-Copy }
    $root = Get-RemoteItem (Get-SwitchFolder) $name
    $sizes = if ($root) { Get-RemoteSizes $root } else { @{} }
    $pending = @($files | Where-Object { $sizes[$_.Rel] -ne $_.Size })
} while ($pending.Count -and (Get-Date) -lt $deadline)
if ($pending.Count) { "TIMEOUT waiting for: $($pending.Rel -join ', ')"; exit 1 }
# Files that keep their size pass the check above before being replaced: wait
# for the copy window to go away too.
while ([MtpConfirm]::Busy([uint32]$PID) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    Confirm-Copy
}
if ([MtpConfirm]::Busy([uint32]$PID)) { "TIMEOUT: the copy is still running"; exit 1 }
# The shell may still be finishing the last write: give it a moment.
Start-Sleep -Seconds 5
if ($NoVerify) { "COPIED: all $($files.Count) files have the right size (not read back)"; exit 0 }

# Read back and compare hashes.
if (Test-Path $VerifyDir) { Remove-Item -Recurse -Force $VerifyDir }
$bad = 0
foreach ($f in $files) {
    $dest = Join-Path $VerifyDir (Split-Path $f.Rel)
    New-Item -ItemType Directory -Force $dest | Out-Null
    $local = Join-Path $dest (Split-Path $f.Rel -Leaf)
    # Same-size files pass the wait above before being replaced: retry the
    # read-back while the old content is still there.
    for ($try = 1; $try -le 6; $try++) {
        if (Test-Path $local) { Remove-Item -Force $local }
        $root = Get-RemoteItem (Get-SwitchFolder) $name
        $item = Get-RemoteItem $root $f.Rel
        if ($item) { $sh.Namespace((Resolve-Path $dest).Path).CopyHere($item, 16) }
        for ($n = 0; $n -lt 240; $n++) {
            if ((Test-Path $local) -and (Get-Item $local).Length -eq $f.Size) { break }
            Start-Sleep -Milliseconds 500
            Confirm-Copy
        }
        Start-Sleep -Milliseconds 500
        $h = if (Test-Path $local) { (Get-FileHash $local -Algorithm SHA256).Hash } else { 'MISSING' }
        if ($h -eq $f.Hash) { break }
        Start-Sleep -Seconds 10
    }
    $ok = $h -eq $f.Hash
    if (-not $ok) { $bad++ }
    "{0}  {1,10}  {2}" -f ($(if ($ok) { 'OK ' } else { 'BAD' })), $f.Size, $f.Rel
}
if ($bad) { "$bad file(s) differ"; exit 1 }
"DONE: all $($files.Count) files verified by SHA-256"
