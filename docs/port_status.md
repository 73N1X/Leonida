# Port status: Leonida, patched from VIctory

Leonida is the base: its runtime (`runtime/`: the `gta6rt` PE loader, the GDK
shims and the D3D12.X-on-Vulkan layer) is what runs the title, and its shader
pipeline (`tools/rdna2spirv.py`, RDNA2 → SPIR-V) is what translates the
shaders. The game renders through **Vulkan**.

DexNom/VIctory (Rust, host Direct3D 12) already runs the title past boot. It is
kept in this repository as a git subtree under `victory/` **only as the source
of patches**: what it learned about the title (loader, system calls, memory
model, GPU behaviour, shader rules) is ported into Leonida's code. Its runtime
is used only as a reference to compare against, never as the shipped runtime.

Nothing from the game is in this repository. Runs read the user's own package
in place; everything derived (shaders, caches, saves, captures, logs) lives in
work folders outside the repository (`G:\gta6port` on the test machine).

## Layout

| Path | What |
|---|---|
| `runtime/`, `tools/`, `make_game.py`, `Build.bat` | Leonida: the product |
| `victory/` | DexNom/VIctory subtree with full history, the patch source. A background poller fetches `victory/main` every 15 minutes and speaks up only when new commits exist; they are then merged with `python -I scripts/sync_victory.py --merge` and reviewed for fixes to port |
| `scripts/sync_victory.py` | Fetches `victory/main`, lists new commits, merges them with `git subtree pull` |
| remote `victory7900` | avatarmanz/VIctory-7900XTX, a second patch source (fetch only, not merged: it diverges from DexNom's line, 408 commits ahead and 334 behind at the time of adding; much of its recent work is frame time). Also polled every 15 minutes in the background. Port from it commit by commit (`git show victory7900/main:<path>`) |
| `tools/victory_saves.py` | Converts a VIctory data folder's saves and local storage into gta6rt's layout, so a save made under VIctory skips the first-boot screens |
| `scripts/port_ctl.py` | Drives a reference run of VIctory's runtime (start, pad presses from a file, frame captures, fps lines, stop) |

## Test machine

Windows 11 Pro 26200, NVIDIA GeForce RTX 4060 Ti 16 GB (driver 616.64,
Vulkan 1.4.351), AMD Ryzen 5 7600X (6 cores, 12 threads), 31 GB RAM. Package:
Xbox Series 1.0.2.1 in `Documents\GTA6` (`make_game.py --check`: all 108
files present).

Vulkan on this GPU: every extension the design needs is exposed:
`VK_EXT_descriptor_buffer`, `VK_EXT_mesh_shader`, `VK_EXT_external_memory_host`
(minImportedHostPointerAlignment 4 KiB), `VK_EXT_shader_object`,
`VK_EXT_graphics_pipeline_library`, `VK_KHR_ray_query`,
`VK_KHR_acceleration_structure`, `VK_EXT_device_generated_commands`,
`VK_KHR_fragment_shader_barycentric`, `VK_EXT_mutable_descriptor_type`,
maintenance5-9, buffer device address, sparse binding. Subgroup size 32 only.
Memory: a 15.7 GiB device-local heap that is also host-visible (resizable BAR
is on), so title GPU memory can live in VRAM and still be written by the CPU.

## Build notes for this machine

- PowerShell finds devkitPro's MSYS2 `git` first (`c:\devkitPro\msys2\usr\bin`),
  and MSYS2 programs expand braces in their arguments: `tag^{commit}` reaches git
  as `tag^commit` and fails (it broke `build_llvm_mc.ps1`'s version check and
  automatic worktree creation). Put `C:\Program Files\Git\cmd` first on PATH
  for any PowerShell script that calls git.
- The `cmake` that PowerShell finds first is devkitPro's MSYS2 build, which has
  no Visual Studio generators. Use the native one:
  `%LOCALAPPDATA%\Programs\CMake\bin\cmake.exe -S runtime -B runtime/build -G "Visual Studio 17 2022" -A x64`.
- The Vulkan device is compiled into `gta6rt` only when Vulkan headers are found
  (Vulkan SDK, or `-DLEONIDA_VULKAN_HEADERS_FETCH=ON`).
- The shader stage needs `llvm-mc` with the AMDGPU target
  (`scripts\build_llvm_mc.ps1`) and `spirv-as` / `spirv-val` (Vulkan SDK).
  `build_llvm_mc.ps1` takes the first `cmake` on PATH, so put the native one
  first: `$env:Path = "$env:LOCALAPPDATA\Programs\CMake\bin;" + $env:Path`.
- Vulkan SDK 1.4.363.1 (LunarG, signed installer, SHA-256
  `C02932BD9A968B22D404536108ADAACAC908C081CD6B0113F5BFEFC523DD0A6A`) installs to
  `C:\VulkanSDK\1.4.363.1`; it needs an elevated (UAC) install.

## Leonida on this machine

- `gta6rt` with the Vulkan device (`runtime\build-vk`, Vulkan SDK 1.4.363.1):
  builds; `run_tiny_frame` (a stand-in title creating the console device on
  Vulkan, clear + readback, through `gta6rt --run`) passes on the RTX 4060 Ti.
- Shader rules merged from `port/shader-rules` (from VIctory): loop iteration
  cap (128 PS / 256 VS,GS,MS / 4096 CS, `--loop-limit`), quad operations and
  helper lanes in pixel shaders, computed EXEC writes and the `tfe`/`lwe`
  status dword. GPU suite after the merge: 230 of 248 pass, 17 skipped; only
  `ms_wave_info_s3` still fails (driver-dependent, see below).
- VS -> PS linking census (`tools\vs_ps_link_census.py`): 323 readable pairs,
  292 link the same by number and by signature row, 31 differ; in 19 the pixel
  shader reads a subset of the vertex outputs, so linking by number reads the
  wrong outputs. Open: link by signature row (docs/design.md lists what it
  needs). Per-program wave64 emulation tiers are open too.
- GPU test suite before the shader rules (`scripts\run_gpu_tests.py`,
  `LEONIDA_W64_EMULATE=1`): 226 of 248 passed, 17 skipped (run by other
  tests, or need 64-wide fragment subgroups), 5 failed:
  - `cs_imgload_tfe`, `cs_imgload_d16_tfe`: the program exits with an error
    (texture-fail status of image loads; partially resident textures use it).
  - `cs_w32_exec_computed` (`8192 8192 8192 7682 512 4100 0 3`) and
    `cs_w64_exec_computed` (`8224 8192 16384 16384 8192 4132 0 3`): wrong
    results with a computed EXEC mask.
  - `ms_wave_info_s3`: (-1,-1,-1,-1) instead of (0.0078,0.0078,0.5,1.0);
    driver-dependent per docs/testing.md.
- Shader stage (`make_game.py --stages shaders`, work folder
  `G:\gta6port\leonida-work`, llvm-mc 21.1.8, 54 min on 6 jobs): all 26,503
  shaders of `shaders.rpf` translated and pass `spirv-val`; 52 of 52
  `XboxBvhBuilder.dll` programs; 18 of 29 FidelityFX (`fspx`) programs (11
  not translated, open). Wave64 emulation is on (32-wide subgroups). Wave64
  VS/PS/GS on 32-wide subgroups: about 5,060 tier A, 100 tier B and 8,250 tier
  C (not exact on this GPU; `wave64_graphics_reasons` in the index lists why):
  the likeliest source of graphical glitches, and what VIctory's per-program
  wave64 handling addresses. 14 GB of SPIR-V.
- Real title on `gta6rt --run`, host D3D12 proxy and `LEONIDA_D3D12=vulkan`
  alike: DirectStorage (merged from `port/dstorage`, ported from VIctory's
  `dstorage.rs`) opens the title's 12 archives; the Vulkan device is created on
  the RTX 4060 Ti (upload heaps in VRAM through resizable BAR, bindless set
  up); then the title crashes about 3 s in at console device slot 26,
  `GetCustomHeapProperties`, which returns a struct by value: the caller passes
  the hidden return pointer in RCX and the proxy reads it as the device
  (`d3d12_xs_device.cpp`, `proxy_slot<26>`). Slot 48 (`CreateCommandQueueX`)
  fails next. Both are with the D3D12 ABI port (`port/d3d12-abi`).
- Audio merged from `port/audio` (ported from VIctory's apu.rs, dsp.rs, audio.rs
  and xma.rs): `xapu.dll`, `chad.dll` and a reworked `acphal.dll`. `gta6rt
  --dry-run` now reports 0 unimplemented imports (11 before). XMA decoding is
  real (FFmpeg's wmaprodec.c by way of VIctory); Opus frames are checked but
  decode to silence (VIctory uses the opus-pure crate, not in the repo); DSP
  reverb is silent, as in VIctory. `SHAPE_CONTEXTS` is now 0x60 bytes with
  32-bit APU addresses, as VIctory asserts; Leonida's 120-byte struct wrote 24
  bytes past the title's (both put `xmaContextArray` at +0x38). With both
  merges the title runs about 11 s before the slot-26 crash.
- BCPack merged from `port/bcpack` (`gta6rt_bcpack`, ported from VIctory's
  victory-formats): bit-exact with VIctory's Rust decoder on all 105,845 BCPack
  chunks of the package (BC1/3/4/5/6H/7, 16.4 GB decoded), 59-61 MB/s per core.
  At run time the format tables come from `LEONIDA_BCPACK_TABLES` (on this
  machine `victory\crates\victory-formats\src\bcpack-tables.bin`, git-ignored)
  or a `bcpack-tables.bin` next to `gta6rt.exe`; without them BCPack requests
  fail with `ERROR_INVALID_DATA`.
- Console D3D12 ABI merged from `port/d3d12-abi` (`runtime\shims\d3d12_xs_slots.h`,
  VIctory's slot tables; Device11 selects the console layouts; struct returns
  take the return buffer first). On Vulkan the title now creates its device,
  queues, descriptor heaps, CBV/SRV/UAV views and its 3840x2160 back buffers
  (device slot 127, CreateCommittedResource3), then stops about 2.7 s in at
  its own breakpoint (`0x80000003`, `game_scarlett_master.exe+0x247af2d`) right
  after device slot 70 (a placed SRV over title memory) returned E_NOTIMPL.
  Next: the title-memory model and the slots that need it (device 70/71, list
  62, 71-74, 100, 148).
- Title memory (`port/title-memory`, `runtime\src\title_memory.h`): a GPU
  address is the title's CPU address. On the RTX 4060 Ti the driver takes
  capture-replay device addresses for imported host memory, so the arena sits
  at heap_base = 512 GiB and every xmem graphics range (now placed in [1, 4)
  GiB for 32-bit memory and [64, 448) GiB otherwise, write-combining dropped as
  VIctory does) is imported at heap_base + its address when committed or
  mapped, and released before the title frees, decommits or remaps it. A
  shader reaches title memory through a V# at a title address in a descriptor
  heap in title memory (`d3d12_title_memory_test`). The retail boot imports
  all of it (4.5 GB of pools, the 32-bit windows, slot 133's 174 MB buffer)
  without a refusal. Device slots 70/71 write V#s at title addresses and make
  textures from title memory (one 2D level of SW_4KB_S or SW_64KB_D detiled;
  other layouts start zeroed, logged); list slots 62, 71, 72, 100 and 148 are
  done, 73/74 waits are checked at ExecuteX with a 100 ms limit, not held.
  The title still stops at its breakpoint `game_scarlett_master.exe+0x247af2d`,
  about 5 s in, right after its two back buffers: a pool check
  (`budget - used + [0x1475E6FF0] <= 0x1FFFFF`, budget 0x5200000 or 0x4000000
  by a console-model test at 0x140075140) that slot 70 does not reach; slot 25
  answers 33,423,360 bytes for each 3840x2160 RGBA8 target.
- Title memory merged from `port/title-memory`: title GPU memory imported into
  Vulkan at its own addresses (capture-replay device addresses, arena at 512
  GiB), device slots 70/71/133 and list slots 62/71/72/100/148 implemented.
  The title then passes slot 70 but still stops at `+0x247af2d`. That
  breakpoint is the title's render-target pool check: it opens a pool (budget
  0x4000000 for Series X, 0x5200000 for Series S, picked by the console model
  it reads from `HKLM\...\OEM\ConsoleMode`), creates its back buffers, and
  requires budget - used + a global to be at most 0x1FFFFF. On the console
  (and in VIctory) committed resources take their memory through the title's
  `__XMemAllocationHooks__`, so the tracker counts them (2 x 33,423,360 bytes
  fill the 64 MiB pool); in Leonida they did not, so `used` stayed 0. The
  registry answer is merged (`runtime\shims\registry_oem.cpp`); routing
  XMemAlloc and committed resources through the title's hooks is in progress
  (`port/xmem-hooks`).
- Presentation (`port/present`, from VIctory's `host.rs`, `queue.rs`, `frame.rs`,
  `dxgi.rs`, `capture.rs` and `lifecycle.rs`): `PresentX` (queue slot 32) on the
  Vulkan device goes to the title's own window (its `CreateWindowExA`, fitted
  to the screen; closing it ends the run) or a window of the runtime's, through
  a swapchain on the presenting queue; the caption shows `Leonida | <fps> fps
  <ms> ms/frame`; the device's frame clock (slots 78-81) hands out the tokens
  and paces, the present does not wait for a vblank; `LEONIDA_CAPTURE_DIR`
  saves requested frames as PNGs off the present path. `run_tiny_present`
  (stand-in title through `gta6rt --run`, screenshot pixels checked) passes on
  the RTX 4060 Ti. The real title still stops before its first present.
- The title makes its window at 3840x2160 and its `ALLOW_DISPLAY` back buffers
  at 3840x2160. It reads the console output's display mode (output slot 21,
  now answered with `LEONIDA_DISPLAY_MODE`, VIctory's `VICTORY_DISPLAY_MODE`)
  just before its first `SetFrameIntervalX`, but 1920x1080 (also at 120 Hz,
  also with `LEONIDA_DEVICE_TYPE=6`) leaves both at 3840x2160. The size is the
  executable's Series X SDR mode: VIctory upstream's native 1440p profile
  (`native_1440p.rs`), ported as `runtime/src/display_profile.h`, patches it
  and three related places in the mapped image, and with
  `LEONIDA_DISPLAY_MODE=2560x1440` the window and both back buffers are
  2560x1440 (`LEONIDA_RENDER_SCALE=0.75` renders 1080p into it). Other sizes
  are refused (1920x1080 still gave 2560x1440 buffers). Its frame-time effect
  is not measured yet: the title stops before its first frame.
- The title's allocator (`port/xmem-hooks`, from VIctory's xmem.rs and
  resource.rs): XMemAlloc/XMemFree call the routines the exe exports as
  `__XMemAllocationHooks__`, and committed resources take their memory from
  them with VIctory's attributes (`title_memory::resource_attributes`), as the
  console's D3D runtime does; a buffer sits on that memory's import, a texture
  reports the address and keeps an image of its own. XMemAllocDefault gives
  graphics memory types pages of their own in the GPU window. The retail
  title's hook takes a 64 MiB render-target pool from XMemAllocDefault
  (attributes 0x30808000), hands both back buffers out of it, and the pool
  check passes. Next, about 5 s in: without `LEONIDA_SHADERS`,
  XboxBvhBuilder.dll throws a C++ exception (exit code 0xE06D7363) under
  `game_scarlett_master.exe+0x2479455`, right after the first device slot 62
  pipeline finds no translated shaders; with them the title runs to about
  33 s and writes to address 0 at `game_scarlett_master.exe+0x2215c35`.
- The title renders to 3840x2160 because it is told it is on a 4K console
  display; reporting a smaller display mode is a large frame-time lever for
  later (VIctory has `VICTORY_DISPLAY_MODE`).
- Console D3D12 slots ported from VIctory on `port/d3d12-slots2` (Vulkan
  device; tests `xs_proxy_mock_test`, `d3d12_sampler2_test`,
  `gfx10_tiling_test` and `d3d12_title_memory_test` pass on the RTX 4060 Ti):
  device 61 CreateSamplerX writes the S# from the console's sampler
  description, and sampler heaps are 16 bytes a slot (views 32); list 73/74
  WaitValue cut the list and the queue holds the rest until the value is in
  memory (another queue's Write32/64, a fence's value word, or the CPU), a
  present forcing what is held; queue 31 CopyPageMappingsBatchX maps
  page-pool pages through xmem (`map_alias`), the title-memory imports
  following the remap; queue 23/24 SuspendX/ResumeX are accepted; device
  70/71 textures are detiled whole (mips, tails, slices, 3D; linear, S and D
  modes, SW_64KB_R_X); Write64's GPU timestamp is a timestamp query the GPU
  writes when it gets there, the system timestamp the performance counter
  once the submission has run. The real title cannot exercise them yet: it
  stops at its render-target pool check before its first frame.
- Time step (`port/frame-timing`, `runtime/src/frame_step.h`): the title's
  timer (+0x22264d0, once a frame) steps its time sets by the measured
  QueryPerformanceCounter delta held to [1/30 s, 1/15 s]: the floor is
  1 / the target frame rate (+0x6849844, from the graphics settings), the cap a
  1/15 constant, both in +0x2226040 and again for the camera's set. Under
  VIctory (game time +0xabbef40, frames +0xabbef44, watched against the wall
  clock): menus at 30 fps 999-1003 game ms a second; the loading screen at
  38-47 fps 1301-1612 (too fast); the world at 0.6-2.4 fps 40-160, i.e. one
  1/15 s step a frame. The patch moves the floor to the timer's minimum step
  (1/3000 s) and the cap to 1 s; `LEONIDA_TIME_STEP=stock` turns it off.
  The same patch written into VIctory's running title: menus 1.000 of real
  time, loading screen at 70 fps 1.000, world at 1.6 fps 1.000 over its frames
  under 1 s (236 of 257; the other 21, 2.6 s on average, step 1 s, so 0.79
  overall against 0.10 before).
- The title asks the device for three interfaces it does not know
  (`{5f6e592d-d895-44c2-8e4a-88ad4926d323}`, `{14eecffc-4df8-40f7-a118-5c816f45695e}`,
  `{5af5c532-4c91-4cd0-b541-15a405395fc5}`); VIctory has no answer for them
  either.

- After the allocator-hooks merge (`a61274951`), with `LEONIDA_SHADERS` set and
  `LEONIDA_DISPLAY_MODE=2560x1440`: the title passes its pool check, makes its
  2560x1440 back buffers from its own allocator, sets its frame interval
  (device slots 78/79), then calls its first CreateComputePipelineState
  (device slot 62); the runtime loads the 26,573-shader index and compiles
  that pipeline on the title's thread, nothing else happens for ~28 s, and
  the title's 30 s hang detector (a thread that counts to 30 and writes to
  address 0 at `+0x2215c35`) ends the run about 36 s in, before the first
  present. Fix: pipelines built off the title's thread (`port/pipelines`).

## Saves for testing

The user's own save (first-time setup and legal screens done, two story
saves), made under VIctory, is kept outside the repository:
- original: `G:\gta6port\saves\manual-save`; working copy for VIctory runs:
  `VICTORY_DATA_DIR=G:\gta6port\data-save`;
- converted for gta6rt: `LEONIDA_SAVE_DIR=G:\gta6port\leonida-saves` with
  `--overlay G:\gta6port\leonida-overlay-save` (its local storage).

## Reference run of VIctory's runtime on this machine (host D3D12)

- Shader warm-up: 25,745 shaders translated, 13 refused; 2,700 compute
  pipelines, 31 refused by the NVIDIA driver (`E_INVALIDARG`): console wave64
  programs whose cross-lane code spans both 32-lane halves keep `WaveSize(64)`,
  which a 32-lane GPU cannot run. Leonida's round-based wave64 emulation
  (docs/design.md) is the fix to carry over.
- Boot, start screen and first-time setup screens: 30 fps, 20-60 draws/frame.
- Stops at the "Legal Terms & Policies" screen: the Terms of Service row is
  skipped by every navigation input (D-pad, left stick, short and long
  presses), so Confirm stays disabled. Unresolved; nothing failing shows in
  the logs. Not reported upstream.
- Two D3D12.X pipeline-creation methods are not implemented (console-ABI
  slots `0x218` and `0x1c8`); the title goes without those pipelines.
- With the user's save (`VICTORY_DATA_DIR=G:\gta6port\data-save`) the setup and
  legal screens are skipped: start screen, A, loading screen, and the world
  (the waterfront house) about 40 s after the press. World frame time on the
  RTX 4060 Ti, first minutes, with Leonida's shader translation running on 6
  CPU threads beside it: 0.4-2.8 fps (355-2,390 ms/frame), 5,000-14,000
  draws/frame, 300-3,300 draws/frame dropped while graphics pipelines are made
  (the warm-up only makes compute pipelines), 70-200 ms/frame preparing on the
  CPU, 12-500 ms on the GPU, ~8.5 GB working set. Visible: stair-stepped,
  blocky edges (very low internal resolution or a broken temporal resolve)
  and black interiors (no ray-traced light). This is the reference Leonida
  has to beat.

## Changes on top of the merged sources

- `victory/`: the stand-in pad command file (`VICTORY_MOVEMENT_FILE`) takes
  every key of the stand-in pad's table, so menus can be driven by a script;
  the user-facing name is Leonida instead of DexNom Runtime.

## Pipelines off the title's thread (`port/pipelines`)

Run on `port/pipelines` merged (uncommitted) with `port/victory-merge` 43f98a6dd, `LEONIDA_D3D12=vulkan`,
`LEONIDA_SHADERS`, `LEONIDA_DISPLAY_MODE=2560x1440`, its own save and overlay folders, no `pipeline_cache.bin`:
the first CreateComputePipelineState (slot 62) returns at once and the title goes on (root signatures, query
heaps, committed resources, views, command signatures), where it stopped ~28 s in that call before. About 12 s in
the title's own int3 at `+0x19f4f58` (a soft assert) ends the run. With `LEONIDA_SKIP_INT3=1` it goes on through
its pipeline burst (six slot-86 streams with no shaders, handed never-made PSOs; over 8,192 slot-57 creations,
refused as VIctory refuses them; the FidelityFX slot-67 programs the shader stage does not translate) and texture
creation, then writes to `0x20` at `+0x24844a9` about 10 s in. No list had bound a pipeline yet, and the frame
clock had begun no frame.

## Open items, in order

1. Build `gta6rt` and run it on the real executable (`--dry-run`, `--init`,
   `--run`); list where it first fails.
2. Port VIctory's loader and system-call fixes into Leonida's shims until the
   title boots and presents frames.
3. Vulkan rendering of menus, then the world: port the GPU-side fixes VIctory
   needed, using Leonida's Vulkan layer.
4. Shaders: install the shader-stage tools, translate the corpus, port the
   translator rules VIctory needed (wave64 on 32-lane GPUs, sampler selection,
   vertex output interpolation and signature order, ...).
5. Frame time: title GPU memory in host-visible VRAM or imported host memory
   with buffer device addresses, instead of per-frame copies. Target 30 fps.
