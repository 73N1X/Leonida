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
