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
- The title renders to 3840x2160 because it is told it is on a 4K console
  display; reporting a smaller display mode is a large frame-time lever for
  later (VIctory has `VICTORY_DISPLAY_MODE`).
- The title asks the device for three interfaces it does not know
  (`{5f6e592d-d895-44c2-8e4a-88ad4926d323}`, `{14eecffc-4df8-40f7-a118-5c816f45695e}`,
  `{5af5c532-4c91-4cd0-b541-15a405395fc5}`); VIctory has no answer for them
  either.

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
