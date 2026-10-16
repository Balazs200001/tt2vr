# start-query-server.ps1 - start the read-only Ghidra query server (TT2QueryServer.java) on the headless project.
# Runs in the foreground until <WorkDir>\stop exists (or 4 h idle). Query it with: node tools/ghidra/gq.js "<command>"
# Stop it with: New-Item <WorkDir>\stop
param(
    [string]$ProjectDir  = 'C:\dev\tt2vr-re\ghidra-headless',
    [string]$ProjectName = 'TT2_4989319',
    [string]$Program     = 'TT2.exe',
    [string]$WorkDir     = 'C:\dev\tt2vr-re\ghidra-query',
    [string]$GhidraDir   = $env:GHIDRA_INSTALL_DIR,
    [string]$MaxMem      = '12G'
)
$ErrorActionPreference = 'Stop'
if (-not $GhidraDir) { throw 'GHIDRA_INSTALL_DIR is not set' }
New-Item -ItemType Directory -Force $WorkDir | Out-Null
Remove-Item -ErrorAction SilentlyContinue (Join-Path $WorkDir 'stop'), (Join-Path $WorkDir 'ready')
$env:GHIDRA_HEADLESS_MAXMEM = $MaxMem
$env:GHIDRA_HEADLESS_JAVA_OPTIONS = '-XX:ParallelGCThreads=6 -XX:CICompilerCount=4'
& (Join-Path $GhidraDir 'support\analyzeHeadless.bat') $ProjectDir $ProjectName `
    -process $Program -noanalysis -readOnly `
    -scriptPath $PSScriptRoot -postScript TT2QueryServer.java $WorkDir `
    -log (Join-Path $WorkDir 'headless.log')
