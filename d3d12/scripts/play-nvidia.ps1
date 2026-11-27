# Starts the title on an NVIDIA card: Series S, the game's own 720p picture taken to 1080p by DLSS, the F1
# settings menu in the game (measured on an RTX 5070: about 15-17 fps before generated frames).
#   scripts\play-nvidia.ps1 -Exe <path of your own copy's executable>
# DLSS and DLSS frame generation need a few NVIDIA files (about 70 MB) that this repository does not carry:
# run Get-NVIDIA-DLSS.cmd in the repository's folder once, and they are fetched from NVIDIA's GitHub into
# nvidia-sdk\. Without them the game runs with its picture stretched to the screen instead.
# The layer is built here if it is not built yet, and built again with DLSS once those files are there.
# In the game: F1 = settings (arrows pick and change, Enter saves; kept in leonida-settings.txt next to this
# folder), F3 = the numbers panel.
# -NoSmoothMotion  run target\<build>\leonida.exe itself. By default a copy named leonida_sm.exe is started instead,
#                  so a Smooth Motion profile set for that exe in the NVIDIA App applies every time.
# -NoDlss          no DLSS: the picture is stretched (fast, softer)
# -SeriesX         the Series X profile instead (much slower; no DLSS there)
# -Quality         fewer stretched/jumping bodies, slower (relay stage 1, no card steps)
# -RayTracing      the title's real ray tracing (the structure builder runs). Shaders build for minutes the
#                  first time, and DLSS frame generation is left off with it (the two together do not work yet).
# -Resolution      the older path: the game renders at this height (2160, 1440, 1080, 720, 540 or 480) and the
#                  title's own upscaler does the rest, without DLSS. The play-S-*.ps1 and play-X-*.ps1 scripts use it.
# -Adapter         on a PC with more than one card, the one to use: part of its name (-Adapter 5060) or its index
#                  (-Adapter '#1'). Default: Windows' default card.
# -Build           target\diag (the default, the build all measurements are of) or target\release
# -DlssDir         the folder with nvngx_dlss.dll, if not nvidia-sdk\DLSS\lib\Windows_x86_64\rel
# -StreamlineDir   the folder with sl.interposer.dll (frame generation), if not nvidia-sdk\Streamline\bin\x64
# -NoPrecompile    start at once. By default every shader of the title is translated into the cache and the
#                  pipelines earlier runs drew with are made before the title starts (minutes the first time
#                  and after a change of the layer, seconds otherwise), so that the world does not come up
#                  half drawn or stall while they are made.
# -Log             the layer's log goes to a file in %LOCALAPPDATA%\Leonida\logs instead of this window
# -Capture         F9 in the game saves that frame and the pictures it drew, in %LOCALAPPDATA%\Leonida\captures
# -WindowsBarMB    maximum MiB of title memory windows in CPU-mapped VRAM (by default 3072 on PCs with under
#                  24 GB of RAM, else 0; -WindowsBarMB 0 turns it off).
# -More            other settings: -More @{LEONIDA_PANEL='1'}
# Run it from a normal window, not as administrator: the driver then keeps its shader cache elsewhere.
param(
    [Parameter(Mandatory)][string]$Exe,
    [switch]$NoSmoothMotion,
    [switch]$NoDlss,
    [switch]$SeriesX,
    [switch]$Quality,
    [switch]$RayTracing,
    [ValidateSet('2160', '1440', '1080', '720', '540', '480')][string]$Resolution,
    [string]$Adapter,
    [ValidateSet('diag', 'release')][string]$Build = 'diag',
    [string]$DlssDir,
    [string]$StreamlineDir,
    [switch]$NoPrecompile,
    [switch]$Log,
    [switch]$Capture,
    [hashtable]$More = @{},
    [uint32]$WindowsBarMB
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not (Test-Path $Exe)) { Write-Host "No $Exe."; return }
$Exe = (Resolve-Path $Exe).Path

# NVIDIA's own files: where they are (Get-NVIDIA-DLSS.cmd puts them in nvidia-sdk\). Nothing is downloaded here.
if (-not $DlssDir) { $DlssDir = $env:LEONIDA_DLSS_DIR }
if (-not $StreamlineDir) { $StreamlineDir = $env:LEONIDA_STREAMLINE_DIR }
$sdkDlss = Join-Path $root 'nvidia-sdk\DLSS'
$sdkDlssLib = Join-Path $sdkDlss 'lib\Windows_x86_64\x64\nvsdk_ngx_d.lib'
$sdkStreamline = Join-Path $root 'nvidia-sdk\Streamline\bin\x64'
$wantDlss = -not $NoDlss -and -not $SeriesX -and -not $Resolution
if (-not $DlssDir -and (Test-Path (Join-Path $sdkDlss 'lib\Windows_x86_64\rel\nvngx_dlss.dll'))) { $DlssDir = Join-Path $sdkDlss 'lib\Windows_x86_64\rel' }
if (-not $StreamlineDir -and (Test-Path (Join-Path $sdkStreamline 'sl.interposer.dll'))) { $StreamlineDir = $sdkStreamline }
if ($wantDlss -and -not $DlssDir) {
    Write-Host 'No DLSS files here: the picture is stretched to the screen instead. Get-NVIDIA-DLSS.cmd (in the'
    Write-Host "repository's folder) fetches them from NVIDIA once, about 70 MB, with frame generation's."
}

# The build: made here if there is none, and made again with DLSS once its SDK is there (the diag build only).
$leonida = Join-Path $root "target\$Build\leonida.exe"
$built = Join-Path $root "target\$Build\built-with.txt"
$hasDlss = (Test-Path $built) -and ((Get-Content $built -Raw) -match 'dlss')
$canDlss = $wantDlss -and (Test-Path $sdkDlssLib)
if ($Build -eq 'diag' -and (-not (Test-Path $leonida) -or ($canDlss -and -not $hasDlss -and (Test-Path $built)))) {
    if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { Write-Host 'No build and no cargo: install Rust (README.md), then start this again.'; return }
    Write-Host 'Building the layer (a few minutes the first time) ...'
    & (Join-Path $PSScriptRoot 'build-nvidia.ps1')
    $hasDlss = (Test-Path $built) -and ((Get-Content $built -Raw) -match 'dlss')
}
if (-not (Test-Path $leonida)) { Write-Host "No $leonida. Build it first (scripts\build-nvidia.ps1)."; return }
# A build nobody noted the making of (cargo by hand) is taken at its word: with DLSS if the files are there.
$dlss = $wantDlss -and $DlssDir -and ($hasDlss -or -not (Test-Path $built))

$settings = @{
    # The play set. Textures are waited for: one loaded once at the start could stay empty for the run otherwise.
    LEONIDA_TEXTURE_NO_WAIT = '0'; LEONIDA_RELAY = '2'; LEONIDA_CARD_STEPS = '2'
    LEONIDA_WIDE_BACK = '1'; LEONIDA_CARD_REHASH_RUNS = '1'
    LEONIDA_JOIN_COPIES = '1'; LEONIDA_INDIRECT_WINDOW = '1'; LEONIDA_RELAY_COPY = '1'
    # Copy-backs caught half-written are fetched again (torn.rs).
    LEONIDA_FETCH_CHECK = '1'
    # Leonida: indirect batches end at every dispatch (INDIRECT_BATCH=1, not 2). With 2 a batch opened before the
    # title's culling and tessellation dispatches clamped their arguments before they were written: the sea's surface
    # and its tiled resolve went missing for a frame (sand or black blocks), 6 times in 271 frames; with 1, none in 441.
    LEONIDA_INDIRECT_BATCH = '1'; LEONIDA_MAP_THREADS = '1'; LEONIDA_TITLE_PRIORITIES = '0'; LEONIDA_CARD_AWAKE = '1'
    # Leonida: the light stand-in stays on while the rays are off (below). Off, with every ray missing, shade went
    # black and the sky white (run n1, 453 frames); on, the same view is lit as on the console. What the title left
    # is kept up to 2 times the estimate (ambient.rs).
    LEONIDA_SHOW_AFTER = '1'; LEONIDA_AMBIENT = '0.8'; LEONIDA_AMBIENT_MOST = '2'; LEONIDA_D3D12_DRED = 'light'
    LEONIDA_THIN = '1'; LEONIDA_JOIN_PARTS = '8'; LEONIDA_UPLOAD_AHEAD = '1'
    # Fog bounded by 1.5 times the light around; a NaN a shader writes is made 0 before blur and the upscaler spread it.
    LEONIDA_AMBIENT_AIR = '1.5'; LEONIDA_NAN_SCRUB = '1'
    # Leonida: the samplers the title's shaders carry in their code filter linearly where they would point-sample.
    # Without it the colour grading came out in bands (a 2000s camera look): the tonemap reads its 33^3 identity
    # grading tables at a log-encoded colour, and read unfiltered they hand back 33 steps over 14 stops, about five
    # flat levels per doubling of brightness (runs p3, p4, c1: 700-1,400 colours in a sky region; run s1 with this:
    # 28,000-32,000). Comparison and anisotropic samplers keep their own filters (pipeline/host.rs static_filter).
    LEONIDA_STATIC_SAMPLERS_LINEAR = '1'
    # Wait for new shaders rather than skip their draws (each pipeline is waited for at most once). The F1 menu's
    # SHADER WAIT MS changes it: 0 never waits, things then show up as their shaders are done.
    LEONIDA_PIPELINE_WAIT_MS = '10000'
    # NVIDIA caps the shader-visible view heap at 1,000,000: without more dynamic slots thousands of draws a frame
    # are dropped (descriptor/host.rs dynamic_slots).
    LEONIDA_DYNAMIC_SLOTS = '524288'
    # Memory compares on the card instead of the CPU.
    LEONIDA_CARD_BUFFERS_MIN_MB = '1'; LEONIDA_CARD_BUFFERS_MB = '1536'
}
$console = if ($SeriesX) { 'series-x' } else { 'series-s' }
if ($Resolution) {
    # The older path: a render scale and the title's own upscaler (rendered height over output height).
    $outHeight = if ($SeriesX) { 2160 } else { 1080 }
    if ([int]$Resolution -gt $outHeight) { Write-Host "That console outputs ${outHeight}p: ${Resolution}p is above it."; return }
    $settings.LEONIDA_RENDER_SCALE = ([int]$Resolution / $outHeight).ToString('0.####', [Globalization.CultureInfo]::InvariantCulture)
    $settings.LEONIDA_OWN_UPSCALER = '1'
} elseif ($dlss) {
    $settings.LEONIDA_UPSCALE = 'dlss'; $settings.LEONIDA_DLSS_DIR = $DlssDir
} elseif (-not $SeriesX) {
    $settings.LEONIDA_UPSCALE = 'stretch'
}
if ($StreamlineDir) { $settings.LEONIDA_STREAMLINE_DIR = $StreamlineDir }
if ($Quality) { $settings.LEONIDA_RELAY = '1'; $settings.LEONIDA_CARD_STEPS = '0' }
if ($RayTracing) {
    # The title's own rays light the picture: no stand-in beside them.
    $settings.LEONIDA_BVH_BUILDER = '1'; $settings.LEONIDA_AMBIENT = '0'; $settings.Remove('LEONIDA_AMBIENT_MOST')
} else {
    # Leonida: ray tracing mostly off, as before the renderer changed. Ray walks end at the root (emit/intersect.rs),
    # and rt_off_hashes.txt lists the 1,000 CS_RaytracingResolve and 34 traced-shadow shaders, which are skipped:
    # run with every ray a miss they made rooms white or black and took the card 33-50 ms a frame (about 10 without).
    # The screen-probe and visibility traces keep running: skipping those too left their outputs as garbage.
    $settings.LEONIDA_BVH_MISS = '1'
    $rtList = Join-Path $PSScriptRoot 'rt_off_hashes.txt'
    if (Test-Path $rtList) { $settings.LEONIDA_SKIP_PIPELINES = (Get-Content $rtList -Raw).Trim() }
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

# What the F1 menu saved (leonida-settings.txt, git-ignored), over the settings above. An empty value unsets one.
$menuFile = Join-Path $root 'leonida-settings.txt'
$smooth = -not $NoSmoothMotion
if (Test-Path $menuFile) {
    foreach ($line in Get-Content $menuFile) {
        if ($line -notmatch '^\s*(LEONIDA_[A-Z0-9_]+)\s*=(.*)$') { continue }
        $k = $Matches[1]; $v = $Matches[2].Trim()
        if ($k -eq 'LEONIDA_MENU_SMOOTH_MOTION') { $smooth = $v -ne '0'; continue }
        if ($k -eq 'LEONIDA_MENU_CONSOLE') { if ($v) { $console = $v }; continue }
        if ($v) { $settings[$k] = $v } else { $settings.Remove($k) }
    }
}
# Leonida: the runs write down the pipelines the title creates (pipeline/known.rs), for the precompile below.
if (-not $NoPrecompile) { $settings.LEONIDA_PIPELINE_BOOK = '1' }
foreach ($k in $More.Keys) { $settings[$k] = $More[$k] }
# DLSS only where it can be: a menu choice of it without the files or the build falls back to the stretch.
if ($settings.LEONIDA_UPSCALE -eq 'dlss' -and -not $dlss) { $settings.LEONIDA_UPSCALE = 'stretch' }
# DLSS frame generation and real ray tracing do not work together yet (0.1 frames a second): with ray tracing
# on, frame generation is left off and Smooth Motion can do the extra frames.
if ($settings.LEONIDA_BVH_BUILDER -eq '1' -and $settings.LEONIDA_FRAME_GEN -and $settings.LEONIDA_FG_WITH_RT -ne '1') {
    Write-Host 'Ray tracing is on: DLSS frame generation is left off (the two together do not work yet).'
    $settings.Remove('LEONIDA_FRAME_GEN')
}
if ($settings.LEONIDA_FRAME_GEN -and -not $StreamlineDir) {
    Write-Host 'Frame generation needs the Streamline files (Get-NVIDIA-DLSS.cmd): left off.'
    $settings.Remove('LEONIDA_FRAME_GEN')
}
# Smooth Motion is a second frame generator: off while DLSS frame generation is on.
if ($settings.LEONIDA_FRAME_GEN -and $settings.LEONIDA_FRAME_GEN -ne '1') { $smooth = $false }
# For the menu: what it shows as the current values, where it saves, and how it starts the game again.
$settings.LEONIDA_MENU_SMOOTH_MOTION = if ($smooth) { '1' } else { '0' }
$settings.LEONIDA_MENU_CONSOLE = $console
$settings.LEONIDA_MENU_FILE = $menuFile
$settings.LEONIDA_MENU_PLAY = $MyInvocation.MyCommand.Path
$again = @("-Exe '$($Exe -replace "'", "''")'", "-Build $Build")
foreach ($s in 'NoSmoothMotion', 'NoDlss', 'SeriesX', 'Quality', 'RayTracing', 'NoPrecompile', 'Log', 'Capture') {
    if ($PSBoundParameters[$s]) { $again += "-$s" }
}
foreach ($s in 'Resolution', 'Adapter', 'DlssDir', 'StreamlineDir') {
    if ($PSBoundParameters[$s]) { $again += "-$s '$($PSBoundParameters[$s] -replace "'", "''")'" }
}
if ($PSBoundParameters.ContainsKey('WindowsBarMB')) { $again += "-WindowsBarMB $WindowsBarMB" }
$settings.LEONIDA_MENU_ARGS = $again -join ' '

$picture = if ($settings.LEONIDA_RENDER_SCALE) { "render scale $($settings.LEONIDA_RENDER_SCALE), the title's upscaler" }
    elseif ($settings.LEONIDA_UPSCALE -eq 'dlss') { 'DLSS' }
    elseif ($settings.LEONIDA_UPSCALE -eq 'stretch') { 'stretched (no DLSS)' }
    else { "the title's upscaler" }
$extras = ''
if ($settings.LEONIDA_FRAME_GEN) { $extras += ", frame generation $($settings.LEONIDA_FRAME_GEN)x" }
if ($settings.LEONIDA_BVH_BUILDER -eq '1') { $extras += ', ray tracing' }
Write-Host "Series $(if ($console -eq 'series-x') { 'X' } else { 'S' }), $picture$extras. F1 in the game: settings."

if ($smooth) {
    # A copy under the name the NVIDIA App's Smooth Motion profile is set for, refreshed when the build changes.
    $sm = Join-Path (Split-Path $leonida) 'leonida_sm.exe'
    if (-not (Test-Path $sm) -or (Get-Item $sm).LastWriteTime -lt (Get-Item $leonida).LastWriteTime) {
        try { Copy-Item $leonida $sm -Force -ErrorAction Stop }
        catch {
            # Still held by a game that has not quite gone: Windows lets it be renamed out of the way.
            Rename-Item $sm "leonida_sm.exe.old-$(Get-Date -Format HHmmss)" -ErrorAction SilentlyContinue
            Copy-Item $leonida $sm -Force
        }
    }
    $leonida = $sm
}
Get-ChildItem (Join-Path (Split-Path $leonida) '*.old-*') -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue

# (Other settings left over in this window go; where your files are stays.)
$paths = 'LEONIDA_CONFIG', 'LEONIDA_GAME_DIR', 'LEONIDA_CACHE_DIR', 'LEONIDA_DATA_DIR'
Get-ChildItem Env:LEONIDA_* | Where-Object { $_.Name -notin $paths } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
foreach ($k in $settings.Keys) { Set-Item "Env:$k" $settings[$k] }
# A shader cache of its own: one an earlier version of this fork filled does not fit this renderer's pipelines.
$toml = Join-Path $root 'leonida.toml'
if (-not $env:LEONIDA_CACHE_DIR -and -not ((Test-Path $toml) -and (Select-String -Path $toml -Pattern '^\s*cache_dir\s*=' -Quiet))) {
    $env:LEONIDA_CACHE_DIR = Join-Path $env:LOCALAPPDATA 'Leonida\cache-nvidia-2'
}
# Leonida: shaders and pipelines are made before the title starts, not while it plays (as Hookshot's release
# builds do). Every shader of the title's shader archive is translated into the cache with the settings of this
# start: once per change of the translator or of a setting a translation depends on; a later start finds them all
# there. Then the host pipelines earlier runs drew with are made in processes of their own, so that the driver's
# cache on disk has them: after a change of the layer or of the driver, and for places first seen in the last run.
if (-not $NoPrecompile) {
    $archive = Join-Path (Split-Path $Exe) 'shaders.rpf'
    $from = if (Test-Path $archive) { $archive } else { Split-Path $Exe }
    Push-Location $root
    # Only what the two commands print themselves (their log goes to the error stream: quiet for these, and no
    # redirection of it, which Windows PowerShell 5.1 turns into errors).
    $logWas = $env:LEONIDA_LOG; $env:LEONIDA_LOG = 'error'
    $stopWas = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        Write-Host 'Precompiling shaders (minutes the first time, seconds after; -NoPrecompile skips it) ...'
        & $leonida shaders $from | Where-Object { $_ -match 'shaders in the cache|do not translate' } |
            ForEach-Object { Write-Host "  $_" }
        Write-Host 'Precompiling the pipelines earlier runs drew with ...'
        & $leonida warm --used | Where-Object { $_ -match 'pipelines known|^\d+ of |made in' } |
            ForEach-Object { Write-Host "  $_" }
    } finally {
        $ErrorActionPreference = $stopWas
        if ($null -eq $logWas) { Remove-Item Env:LEONIDA_LOG -ErrorAction SilentlyContinue } else { $env:LEONIDA_LOG = $logWas }
        Pop-Location
    }
}
$stamp = Get-Date -Format 'MMdd-HHmmss'
if ($Capture) {
    $env:LEONIDA_CAPTURE = Join-Path $env:LOCALAPPDATA "Leonida\captures\$stamp"
    $env:LEONIDA_DUMP_TARGETS = '1'
    New-Item -ItemType Directory -Force $env:LEONIDA_CAPTURE | Out-Null
    Write-Host "F9 saves a frame to $env:LEONIDA_CAPTURE"
}
$runArgs = @('run', "`"$Exe`"", '--execute', '--console', $console)
Push-Location $root   # leonida.toml is looked for here
try {
    if ($Log) {
        $logs = Join-Path $env:LOCALAPPDATA 'Leonida\logs'
        New-Item -ItemType Directory -Force $logs | Out-Null
        Get-ChildItem $logs -Filter 'play-*.log' | Sort-Object LastWriteTime -Descending | Select-Object -Skip 4 | Remove-Item -ErrorAction SilentlyContinue
        Write-Host "log: $logs\play-$stamp.log"
        Start-Process -FilePath $leonida -ArgumentList $runArgs -NoNewWindow -Wait -RedirectStandardError "$logs\play-$stamp.log"
    } else {
        Start-Process -FilePath $leonida -ArgumentList $runArgs -NoNewWindow -Wait
    }
} finally { Pop-Location }
