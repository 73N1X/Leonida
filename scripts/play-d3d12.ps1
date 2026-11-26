# Starts the title on Leonida's D3D12 runtime (d3d12/, from trustno1-coder/VIctory-NVIDIA since bafb0e25b) on Windows.
# The Vulkan renderer stays scripts\play.ps1; Linux uses scripts\play-linux.sh (docs/linux.md).
#
#   powershell -ExecutionPolicy Bypass -File scripts\play-d3d12.ps1 -Game <folder with game_scarlett_master.exe>
#
# Builds d3d12/ (diag profile, the one d3d12/scripts/play-nvidia.ps1 measures) when it is missing, keeps the caches
# and saves under -Data, and hands everything else to victory\scripts\play-nvidia.ps1: its measured NVIDIA settings,
# -SeriesX, -Quality, -Resolution, -Adapter and -More pass through. Nothing goes online.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Data = (Join-Path $env:LOCALAPPDATA "Leonida\d3d12"),
    [switch]$SeriesX,
    [switch]$Quality,
    [ValidateSet('', 'native', '2160', '1440', '1080', '720', '540', '480')][string]$Resolution = '',
    [string]$Adapter = '',
    [hashtable]$More = @{},
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repo "d3d12"
$exe = Join-Path $runtime "target\diag\leonida.exe"
$title = Join-Path $Game "game_scarlett_master.exe"
if (-not (Test-Path $title)) { throw "missing: $title" }
if ($Rebuild -or -not (Test-Path $exe)) {
    Push-Location $runtime
    try {
        cargo build --profile diag -p leonida --features diagnostics
        if ($LASTEXITCODE -ne 0) { throw "cargo build failed ($LASTEXITCODE)" }
    } finally { Pop-Location }
}
New-Item -ItemType Directory -Force (Join-Path $Data "cache"), (Join-Path $Data "data") | Out-Null
$env:LEONIDA_GAME_DIR = (Resolve-Path $Game).Path
$env:LEONIDA_CACHE_DIR = Join-Path $Data "cache"
$env:LEONIDA_DATA_DIR = Join-Path $Data "data"

$launch = @{ Exe = $title; Build = 'diag'; NoSmoothMotion = $true; More = $More }
if ($SeriesX) { $launch.SeriesX = $true }
if ($Quality) { $launch.Quality = $true }
if ($Resolution) { $launch.Resolution = $Resolution }
if ($Adapter) { $launch.Adapter = $Adapter }
& (Join-Path $runtime "scripts\play-nvidia.ps1") @launch
