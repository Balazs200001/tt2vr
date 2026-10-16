<#
.SYNOPSIS
  Sends a remote request to the running mod and waits for the result.

.DESCRIPTION
  Writes <TT2_GAME_DIR>\tt2vr\commands\<id>.txt (atomically: .tmp then rename). The mod polls the folder
  every [commands] poll_frames frames, deletes the request, runs it and writes <id>.done with one
  result line per command. This script prints those lines.

  Commands (one per line, several allowed):
    frametrace          trace the next frame        -> captures\frametrace_<frame>.txt (+ shader_registry.txt)
    dump                texture dump of next frame  -> captures\<frame>_dump.txt + PNG/DDS files
    capture [name]      RenderDoc capture (game launched through RenderDoc) -> <renderdoc_dir>\<name>_frame<N>.rdc
    shaders             write captures\shader_registry.txt
    camera              log the main view camera (fov, near/far, world matrix, projection) -> tt2vr.log

.EXAMPLE
  powershell -File tools\tt2vr-request.ps1 frametrace dump
  powershell -File tools\tt2vr-request.ps1 "capture snaefell_helmet"
#>
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]] $Commands,
    [int] $TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
if (-not $env:TT2_GAME_DIR) { throw 'TT2_GAME_DIR is not set' }
$dir = Join-Path $env:TT2_GAME_DIR 'tt2vr\commands'
New-Item -ItemType Directory -Force $dir | Out-Null

$id = 'req_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff')
$tmp = Join-Path $dir "$id.tmp"
$req = Join-Path $dir "$id.txt"
$done = Join-Path $dir "$id.done"

Set-Content -Path $tmp -Value ($Commands -join "`n") -Encoding ascii
Move-Item -Path $tmp -Destination $req

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    if (Test-Path $done) {
        Start-Sleep -Milliseconds 100   # let the writer finish
        Get-Content $done
        Remove-Item $done
        exit 0
    }
    Start-Sleep -Milliseconds 250
}

if (Test-Path $req) {
    Remove-Item $req
    Write-Error "No response after $TimeoutSeconds s and the request was never picked up: is the game running with the mod, and is it presenting frames (not minimised)?"
}
else {
    Write-Error "Request was picked up but no result after $TimeoutSeconds s (a dump or capture can take a while; check tt2vr\logs\tt2vr.log)."
}
exit 1
