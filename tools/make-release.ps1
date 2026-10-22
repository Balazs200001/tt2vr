# Builds the player download: build\release\tt2vr-<version>.zip (version from src/core/version.hpp), to be extracted
# into the game folder (the one with TT2.exe):
#   dinput8.dll                     the loader, next to TT2.exe
#   tt2vr\tt2vr.dll, tt2vr.toml     the mod and its settings
#   tt2vr\README.md, LICENSE.txt, THIRD-PARTY-NOTICES.txt
# Usage: tools/make-release.ps1 [-SkipBuild]   (default: Release build and unit tests first, presets as in README)
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$version = [regex]::Match((Get-Content -Raw "$root\src\core\version.hpp"), 'kVersion = "([^"]+)"').Groups[1].Value
if (-not $version) { throw 'kVersion not found in src/core/version.hpp' }

if (-not $SkipBuild) {
    Push-Location $root
    try {
        cmake --build --preset x64-release
        if ($LASTEXITCODE -ne 0) { throw 'build failed' }
        ctest --preset x64-release
        if ($LASTEXITCODE -ne 0) { throw 'unit tests failed' }
    }
    finally { Pop-Location }
}

$bin = "$root\build\Release"
$share = "$root\build\vcpkg_installed\x64-windows-static\share"
$out = "$root\build\release"
$zip = "$out\tt2vr-$version.zip"

# Licenses of everything linked into tt2vr.dll (vcpkg's copyright files, the vendored headers' own texts). The MIT,
# BSD and Apache terms ask for these notices in binary distributions.
$components = @(
    @('MinHook', "$share\minhook\copyright"),
    @('spdlog', "$share\spdlog\copyright"),
    @('{fmt}', "$share\fmt\copyright"),
    @('toml++', "$share\tomlplusplus\copyright"),
    @('Dear ImGui', "$share\imgui\copyright"),
    @('DirectXTK', "$share\directxtk\copyright"),
    @('DirectXMath', "$share\directxmath\copyright"),
    @('xxHash', "$share\xxhash\copyright"),
    @('OpenXR SDK loader', "$share\openxr-loader\copyright"),
    @('JsonCpp (used by the OpenXR loader)', "$share\jsoncpp\copyright"),
    @('AMD FidelityFX Super Resolution 1.0', "$root\third_party\fidelityfx-fsr1\license.txt")
)
$notices = New-Object System.Text.StringBuilder
[void]$notices.AppendLine("tt2vr $version includes the following third-party code.")
foreach ($c in $components) {
    if (-not (Test-Path $c[1])) { throw "license file missing: $($c[1]) (configure the x64-release preset first)" }
    [void]$notices.AppendLine().AppendLine("==== $($c[0]) ====").AppendLine()
    [void]$notices.AppendLine((Get-Content -Raw -Encoding UTF8 $c[1]).Trim())
}
$renderdoc = Get-Content "$root\third_party\renderdoc\renderdoc_app.h" -TotalCount 23 # its license comment
[void]$notices.AppendLine().AppendLine('==== RenderDoc in-application API header ====').AppendLine()
[void]$notices.AppendLine(($renderdoc | ForEach-Object { ($_ -replace '^\s*/?\*+/?\s?', '').TrimEnd() }) -join "`r`n")

$utf8 = New-Object System.Text.UTF8Encoding($false)
$files = @(
    @("$bin\dinput8.dll", 'dinput8.dll'),
    @("$bin\tt2vr.dll", 'tt2vr/tt2vr.dll'),
    @("$root\config\tt2vr.toml", 'tt2vr/tt2vr.toml'),
    @("$root\README.md", 'tt2vr/README.md'),
    @("$root\LICENSE", 'tt2vr/LICENSE.txt')
)
foreach ($f in $files) {
    if (-not (Test-Path $f[0])) { throw "missing: $($f[0])" }
}

New-Item -ItemType Directory -Force $out | Out-Null
if (Test-Path $zip) { Remove-Item $zip }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# Entries named with '/' (PowerShell 5.1's Compress-Archive writes '\', which some unzip tools treat as file names).
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in $files) {
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f[0], $f[1], 'Optimal')
    }
    $writer = New-Object System.IO.StreamWriter($archive.CreateEntry('tt2vr/THIRD-PARTY-NOTICES.txt').Open(), $utf8)
    try { $writer.Write($notices.ToString()) } finally { $writer.Dispose() }
}
finally { $archive.Dispose() }

$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
"$zip"
"{0:N0} bytes, SHA-256 {1}" -f (Get-Item $zip).Length, $hash
