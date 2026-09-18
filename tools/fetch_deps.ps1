# Fetches XPMP2 into lib\XPMP2. XPMP2 ships a complete X-Plane SDK in
# lib\XPMP2\lib\SDK, so this is the only dependency you need to build.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\fetch_deps.ps1
$ErrorActionPreference = "Stop"

$Root     = Split-Path -Parent $PSScriptRoot
$Xpmp2Dir = Join-Path $Root "lib\XPMP2"
$Tag      = if ($env:XPMP2_TAG) { $env:XPMP2_TAG } else { "v3.6.1" }

New-Item -ItemType Directory -Force -Path (Join-Path $Root "lib") | Out-Null

if (Test-Path (Join-Path $Xpmp2Dir ".git")) {
    Write-Host "XPMP2 already present in lib\XPMP2 -- updating to $Tag"
    git -C $Xpmp2Dir fetch --tags --depth 1 origin $Tag
    git -C $Xpmp2Dir checkout -q $Tag
} else {
    Write-Host "Cloning XPMP2 $Tag into lib\XPMP2"
    git clone --depth 1 --branch $Tag https://github.com/TwinFan/XPMP2.git $Xpmp2Dir
}

if (-not (Test-Path (Join-Path $Xpmp2Dir "lib\SDK\CHeaders\XPLM\XPLMPlugin.h"))) {
    Write-Warning "No X-Plane SDK inside XPMP2. Download it from"
    Write-Warning "https://developer.x-plane.com/sdk/plugin-sdk-downloads/ and use -DXPLANE_SDK=<path>."
}

Write-Host ""
Write-Host "Done. Now build with:"
Write-Host "  cmake -B build -G `"Visual Studio 17 2022`" -A x64"
Write-Host "  cmake --build build --config Release"
