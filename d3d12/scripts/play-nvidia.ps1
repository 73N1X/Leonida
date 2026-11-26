# Starts the title on an NVIDIA card with the Series S settings measured on an RTX 5070 (about 18-22 fps, no device loss).
#   scripts\play-nvidia.ps1 -Exe <path of your own copy's executable>
# -NoSmoothMotion  run target\<build>\leonida.exe itself. By default a copy named leonida_sm.exe is started instead,
#                  so a Smooth Motion profile set for that exe in the NVIDIA App applies every time.
# -SeriesX         the Series X profile instead (slower: about 7 fps at half scale)
# -Quality         fewer stretched/jumping bodies and less rainbow skin, at about 13 fps instead of about 20
# -RayTracing      keep the title's ray-tracing passes (off by default: see below)
# -Resolution      the height the game renders at before the title's own upscaler: 2160, 1440, 1080, 720, 540 or 480.
#                  Series S outputs 1080p (so up to 1080 there), Series X 2160p. Default: half the output (540 on S,
#                  1080 on X). The play-S-*.ps1 and play-X-*.ps1 scripts next to this one call it with each.
# -Adapter         on a PC with more than one card, the one to use: part of its name (-Adapter 5060) or its index
#                  (-Adapter '#1'). Default: Windows' default card (the one the main display is on).
# -Build           target\diag (the default, the build all measurements are of) or target\release
#                  (the release build ignores the ray-tracing skip list: LEONIDA_SKIP_PIPELINES is a diagnostics switch)
# -WindowsBarMB    maximum MiB of title memory windows in CPU-mapped VRAM (by default 3072 on PCs with under
#                  24 GB of RAM, else 0; -WindowsBarMB 0 turns it off).
# -More            other settings: -More @{LEONIDA_PANEL='1'}
# Run it from a normal window, not as administrator: the driver then keeps its shader cache elsewhere.
param(
    [Parameter(Mandatory)][string]$Exe,
    [switch]$NoSmoothMotion,
    [switch]$SeriesX,
    [switch]$Quality,
    [switch]$RayTracing,
    [ValidateSet('2160', '1440', '1080', '720', '540', '480')][string]$Resolution,
    [string]$Adapter,
    [ValidateSet('diag', 'release')][string]$Build = 'diag',
    [hashtable]$More = @{},
    [uint32]$WindowsBarMB
)
$settings = @{
    # This fork's play set (scripts\play.ps1, not -Safe): the speed set.
    LEONIDA_TEXTURE_NO_WAIT = '1'; LEONIDA_RELAY = '2'; LEONIDA_CARD_STEPS = '2'
    LEONIDA_WIDE_BACK = '1'; LEONIDA_CARD_REHASH_RUNS = '1'
    LEONIDA_JOIN_COPIES = '1'; LEONIDA_INDIRECT_WINDOW = '1'; LEONIDA_RELAY_COPY = '1'
    LEONIDA_INDIRECT_BATCH = '2'; LEONIDA_MAP_THREADS = '1'; LEONIDA_TITLE_PRIORITIES = '0'; LEONIDA_CARD_AWAKE = '1'
    LEONIDA_SHOW_AFTER = '1'; LEONIDA_AMBIENT = '0.8'; LEONIDA_D3D12_DRED = 'light'; LEONIDA_SKIP_PIPELINES = 'none'
    LEONIDA_THIN = '1'; LEONIDA_JOIN_PARTS = '8'; LEONIDA_UPLOAD_AHEAD = '1'
    # Cheap picture fixes, on in both sets:
    # - Overbright views: without rays the light probes keep the sky's light indoors. The fog is bounded by 1.5 times
    #   the light around, and what the title left by 2 times the estimate (was 4; ambient.rs).
    LEONIDA_AMBIENT_AIR = '1.5'; LEONIDA_AMBIENT_MOST = '2'
    # - Black blobs: a NaN a shader writes is made 0 before blur and the temporal upscaler spread it (emit/lower.rs).
    LEONIDA_NAN_SCRUB = '1'
    # Wait for new shaders rather than skip their draws (a hitch the first time a place is seen). Each pipeline is
    # waited for at most once (LEONIDA_PIPELINE_WAIT_ONCE, on by default): one that takes longer no longer stops the
    # picture in every frame it is in view (pipeline/make.rs).
    LEONIDA_PIPELINE_WAIT_MS = '10000'
    # NVIDIA caps the shader-visible view heap at 1,000,000: without more dynamic slots thousands of draws a frame
    # are dropped (descriptor/host.rs dynamic_slots).
    LEONIDA_DYNAMIC_SLOTS = '524288'
    # Half render scale with the title's own upscaler (Series S needs it with a scale below 1; leonida-loader hooks.rs).
    LEONIDA_RENDER_SCALE = '0.5'; LEONIDA_OWN_UPSCALER = '1'
    # Ray walks end at the root while the structure builder is off (emit/intersect.rs).
    LEONIDA_BVH_MISS = '1'
    # Memory compares on the card instead of the CPU: 11 -> about 22 fps on Series S.
    LEONIDA_CARD_BUFFERS_MIN_MB = '1'; LEONIDA_CARD_BUFFERS_MB = '1536'
    # Series S presents the interface as a second plane: laid over the game (compose.rs), else black before gameplay.
    LEONIDA_COMPOSE = '1'
}
# Ray tracing mostly off. The structure builder cannot run on NVIDIA (its 64-lane programs are refused; upstream
# Marvisak/VIctory pull request 2311), so every ray misses, and the passes that turn misses into light made rooms go
# white depending on the view angle. rt_off_hashes.txt lists the 1,000 CS_RaytracingResolve and 34 traced-shadow
# shaders, which are skipped. The screen-probe and visibility traces keep running: skipping those too left their
# outputs as garbage (rainbow glitter everywhere).
$rtList = Join-Path $PSScriptRoot 'rt_off_hashes.txt'
if (-not $RayTracing -and (Test-Path $rtList)) {
    $settings.LEONIDA_SKIP_PIPELINES = (Get-Content $rtList -Raw).Trim()
}
if ($Quality) {
    # Stretched/jumping bodies: relay stage 1 and no card steps (avatarmanz/VIctory-7900XTX issue 13,
    # fxceless/victory-fork issue 89). Rainbow skin: texture refreshes wait for the runtime's remapping.
    # Costs about 7 fps (the submitting thread waits about 30 ms a frame for the card).
    $settings.LEONIDA_RELAY = '1'; $settings.LEONIDA_CARD_STEPS = '0'; $settings.LEONIDA_TEXTURE_NO_WAIT = '0'
}
# The render scale is the rendered height over the output height (Series S 1080, Series X 2160).
$outHeight = if ($SeriesX) { 2160 } else { 1080 }
$console = if ($SeriesX) { 'X' } else { 'S' }
if ($Resolution) {
    if ([int]$Resolution -gt $outHeight) {
        Write-Host "Series $console outputs ${outHeight}p: ${Resolution}p is above it."
        return
    }
    $settings.LEONIDA_RENDER_SCALE = ([int]$Resolution / $outHeight).ToString('0.####', [Globalization.CultureInfo]::InvariantCulture)
}
if ($Adapter) { $settings.LEONIDA_ADAPTER = $Adapter }
if ($PSBoundParameters.ContainsKey('WindowsBarMB')) {
    $settings.LEONIDA_WINDOWS_BAR_MB = $WindowsBarMB.ToString([Globalization.CultureInfo]::InvariantCulture)
} else {
    # Without -WindowsBarMB, PCs with under 24 GB of RAM get 3072 (PR #6). Windows lets a card share at most half
    # the RAM, and the windows filled that on a 16 GB PC (RTX 3060) about a minute into the world (0x8007000E, then
    # a driver fault). 32 GB and more never got there, so they keep the runtime default (0, off).
    $ram = try { (Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).TotalPhysicalMemory } catch { 0 }
    if ($ram -gt 0 -and $ram -lt 24GB) {
        $settings.LEONIDA_WINDOWS_BAR_MB = '3072'
        Write-Host "$([math]::Round($ram / 1GB)) GB of RAM: title memory windows in the card's memory the CPU maps, up to 3 GB (-WindowsBarMB 0 turns this off)"
    }
}
foreach ($k in $More.Keys) { $settings[$k] = $More[$k] }
$scale = [double]::Parse($settings.LEONIDA_RENDER_SCALE, [Globalization.CultureInfo]::InvariantCulture)
Write-Host "Series $console, rendering at $([int]($outHeight * $scale))p (render scale $($settings.LEONIDA_RENDER_SCALE))"

$leonida = Join-Path $PSScriptRoot "..\target\$Build\leonida.exe"
if (-not (Test-Path $leonida)) { Write-Host "No $leonida. Build it first (README.md)."; return }
if (-not (Test-Path $Exe)) { Write-Host "No $Exe."; return }
if (-not $NoSmoothMotion) {
    # A copy under the name the NVIDIA App's Smooth Motion profile is set for, refreshed when the build changes.
    $sm = Join-Path (Split-Path $leonida) 'leonida_sm.exe'
    if (-not (Test-Path $sm) -or (Get-Item $sm).LastWriteTime -lt (Get-Item $leonida).LastWriteTime) {
        Copy-Item $leonida $sm -Force
    }
    $leonida = $sm
}
# (Other settings left over in this window go; where your files are stays.)
$paths = 'LEONIDA_CONFIG', 'LEONIDA_GAME_DIR', 'LEONIDA_CACHE_DIR', 'LEONIDA_DATA_DIR'
Get-ChildItem Env:LEONIDA_* | Where-Object { $_.Name -notin $paths } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
foreach ($k in $settings.Keys) { Set-Item "Env:$k" $settings[$k] }
$Exe = (Resolve-Path $Exe).Path
$runArgs = @('run', $Exe, '--execute')
if (-not $SeriesX) { $runArgs += @('--console', 'series-s') }
Push-Location (Join-Path $PSScriptRoot '..')   # leonida.toml is looked for here
try { & $leonida @runArgs } finally { Pop-Location }
