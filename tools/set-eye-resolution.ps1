<#
.SYNOPSIS
  Makes TT2 render at the headset's per-eye resolution (PLAN Phase 3), or restores the previous settings.

.DESCRIPTION
  Edits <Documents>\My Games\TT Isle of Man 2\UserSettings.cfg (the game's own settings file, not the install):
  Engine.Device.ScreenMode = "Windowed", ResolutionWidth/Height = the eye size, Maximized = false.
  The first run saves the original file as UserSettings.cfg.tt2vr-backup; -Restore copies it back.
  The game must be closed (it rewrites the file on exit). The eye size is in tt2vr\logs\tt2vr.log
  ("run tools/set-eye-resolution.ps1 -Width W -Height H") and in the overlay's Headset section.

.EXAMPLE
  powershell -File tools\set-eye-resolution.ps1 -Width 1832 -Height 1920
  powershell -File tools\set-eye-resolution.ps1 -Restore
#>
param(
    [int] $Width = 0,
    [int] $Height = 0,
    [switch] $Restore
)

$ErrorActionPreference = 'Stop'
if (Get-Process -Name 'TT2' -ErrorAction SilentlyContinue) { throw 'Close the game first (it rewrites UserSettings.cfg on exit).' }

$dir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\TT Isle of Man 2'
$file = Join-Path $dir 'UserSettings.cfg'
$backup = "$file.tt2vr-backup"

if ($Restore) {
    if (-not (Test-Path $backup)) { throw "No backup at $backup" }
    Copy-Item $backup $file -Force
    Remove-Item $backup
    Write-Output "Restored $file from the backup."
    return
}
if ($Width -lt 16 -or $Height -lt 16) { throw 'Give -Width and -Height (the eye size from the log), or -Restore.' }
if (-not (Test-Path $file)) { throw "$file not found: change any video option in the game once so it writes the file." }
if (-not (Test-Path $backup)) {
    Copy-Item $file $backup
    Write-Output "Backup: $backup"
}

$values = [ordered]@{
    'Engine.Device.ScreenMode'       = '"Windowed"'
    'Engine.Device.Maximized'        = 'false'
    'Engine.Device.ResolutionWidth'  = "$Width"
    'Engine.Device.ResolutionHeight' = "$Height"
}
$lines = [System.Collections.Generic.List[string]]::new()
$seen = @{}
foreach ($line in [IO.File]::ReadAllLines($file)) {
    $key = ($line -split '=', 2)[0].Trim()
    if ($values.Contains($key)) {
        $lines.Add("$key = $($values[$key]);")
        $seen[$key] = $true
    }
    else {
        $lines.Add($line)
    }
}
foreach ($key in $values.Keys) {
    if (-not $seen.ContainsKey($key)) { $lines.Add("$key = $($values[$key]);") }
}
[IO.File]::WriteAllLines($file, $lines, [Text.UTF8Encoding]::new($false))
Write-Output "Set Windowed ${Width}x${Height} in $file"
