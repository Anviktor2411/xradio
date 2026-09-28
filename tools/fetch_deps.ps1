# Fetches the third-party code the plugin builds against, into lib\:
#   XPMP2      -- CSL models, TCAS, map (also ships a complete X-Plane SDK)
#   opus       -- the voice codec
#   miniaudio  -- microphone and speaker access, one header
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\fetch_deps.ps1
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force -Path (Join-Path $Root "lib") | Out-Null

function Fetch($Name, $Url, $Tag) {
    $Dir = Join-Path $Root "lib\$Name"
    if (Test-Path (Join-Path $Dir ".git")) {
        Write-Host "${Name}: already present, updating to $Tag"
        git -C $Dir fetch --tags --depth 1 origin $Tag
        git -C $Dir checkout -q $Tag
    } else {
        Write-Host "${Name}: cloning $Tag"
        git clone --depth 1 --branch $Tag $Url $Dir
    }
}

$XpmpTag = if ($env:XPMP2_TAG)     { $env:XPMP2_TAG }     else { "v3.6.1" }
$OpusTag = if ($env:OPUS_TAG)      { $env:OPUS_TAG }      else { "v1.5.2" }
$MaTag   = if ($env:MINIAUDIO_TAG) { $env:MINIAUDIO_TAG } else { "0.11.25" }

Fetch "XPMP2"     "https://github.com/TwinFan/XPMP2.git"     $XpmpTag
Fetch "opus"      "https://github.com/xiph/opus.git"         $OpusTag
Fetch "miniaudio" "https://github.com/mackron/miniaudio.git" $MaTag

if (-not (Test-Path (Join-Path $Root "lib\XPMP2\lib\SDK\CHeaders\XPLM\XPLMPlugin.h"))) {
    Write-Warning "No X-Plane SDK inside XPMP2. Download it from"
    Write-Warning "https://developer.x-plane.com/sdk/plugin-sdk-downloads/ and use -DXPLANE_SDK=<path>."
}

Write-Host ""
Write-Host "Done. Now build with:"
Write-Host "  cmake -B build -G `"Visual Studio 17 2022`" -A x64"
Write-Host "  cmake --build build --config Release"
