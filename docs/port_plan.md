# Port plan: what to take from VIctory into Leonida, in order

Leonida's runtime is the base; VIctory (`victory/`) is the reference that has
run the title. This plan lists what the title needs that Leonida's runtime does
not do yet, in the order the title hits it, and where each fix lands.
Path roots: `L\` = `runtime\`, `V\` = `victory\crates\victory-gpu\src\`,
`VH\` = `victory\crates\victory-hle\src\modules\`.

## Measured so far (`gta6rt` on the 1.0.2.1 package)

- `--dry-run`, `--init`: all five modules map and initialise (TLS callbacks,
  DLL entry points). 11 imports were unimplemented:
  `XCRDAPI.dll!DStorageGetFactory`, `xapu.dll!XApu{Connect,DequeueResult,
  Disconnect,EnqueueCommand,GetResultCount}`, `chad.dll!XDsp{Activate,
  ConnectWithMaximumStreamLimit,Deactivate,Disconnect,SubmitCommand}`. With
  the audio port (`L\shims\xapu.cpp`, `L\shims\chad_dsp.cpp`, the coprocessor
  and XMA decoder in `L\shims\acphal.cpp`) the dry run leaves only
  `DStorageGetFactory`.
- `--run`: the GDK runtime initialises offline, then the title calls
  `DStorageGetFactory` and stops (about 3 s in).

## Step 1: system calls the title needs before its first frame

| What | VIctory source | Lands in |
|---|---|---|
| DirectStorage (factory, queues, requests, status arrays, zlib, BCPack) | `VH\dstorage.rs` | new `L\shims\dstorage_xs.cpp`; inflate from `L\src\shader_library.h` |
| BCPack decode (tables supplied locally, SHA-256 checked, never committed) | `victory\crates\victory-formats\src\bcpack*` | new `L\src\bcpack.h/.cpp` |
| APU (`xapu.dll`) and DSP (`chad.dll`) | `VH\apu.rs`, `VH\dsp.rs`, `VH\audio\` | done: `L\shims\xapu.cpp` (Opus decoded as silence: no decoder in the repo), `L\shims\chad_dsp.cpp`, coprocessor and XMA in `L\shims\acphal.cpp`, `L\src\xma_decode.h` |

## Step 2: the console D3D12 ABI in front of the Vulkan layer

The title creates its device as `ID3D12Device11`, which selects the newer
console layouts: list slots from 59 and queue slots from 14 are two slots later
(`V\device.rs:149-168`, `V\list.rs:2134`, `V\queue.rs:985`). Leonida's list map
is half on the newer layout and its queue and fence proxies use PC order
(`L\shims\d3d12_xs_device.cpp:623-633, 681`).

Boot, in order:

| # | Title calls (VIctory reference) | Leonida today |
|---|---|---|
| 1 | Queue Signal 16, Wait 17, GetTimestampFrequency 18 (`queue.rs:960-987`) | forwarded as PC 16/17/18 |
| 2 | Fence slot 12: address of the 64-bit value the title polls (`fence.rs:21-27,136-139`) | refused |
| 3 | Device 69 (debug filter, no-op), 127 CreateCommittedResource3 for back buffers (`device.rs:442,457`; `resource.rs:2162-2186`) | 69 → PC CreateCommittedResource2; 127 E_NOTIMPL |
| 4 | Queue 35 ExecuteX, 34 Signal2X, 32 PresentX (`queue.rs:597-610,757-841`) | E_NOTIMPL; `XboxPresentation` exists unrouted (`L\shims\xbox_present.h`) |
| 5 | Device 78-81 frame clock (`frame.rs:1-130`) | 78-80 return at once |
| 6 | Lists from device slot 12 get the console layout (`list.rs:566`) | returned unwrapped (`d3d12_xs_device.cpp:1819-1830`) |
| 7 | List 139 ResetX, 80 DispatchX, 71/72 Write32/64, 73/74 WaitValue, 62 CopyMemory, 86 CopyResourceX, 148 AllocateUploadMemory (`list.rs:2059-2149`) | return 0 (`d3d12_xs_device.cpp:1062-1071`, map `d3d12_xs_device.h:172-182`) |
| 8 | Queries at slots 28-30 (`list.rs:2114-2117,1330-1345`) | dropped; 56-59 shifted by a guessed +4 (59 is a marker per `docs\d3d12_command_objects.md:565`) |
| 9 | ~60,000 PSOs in the first 20 s; device 86 CreatePipelineState with streams (`pipeline.rs:6-11`) | every PSO compiled at creation (`d3d12_vulkan_device.cpp:3948,4061`), slot 86 E_NOTIMPL; the title's 30 s watchdog fires |

Menus and movies: sampler stride 16, views 32 (`descriptor.rs:1-7`; Leonida
uses 32 for all, `L\shims\d3d12_com.h:418`); device 61 CreateSamplerX; device
70/71 views over title memory (`descriptor.rs:795-807`, `resource.rs:2383-2450`);
textures the title writes, detiled from title memory (`texture.rs`); vertex,
index, constant and root views at title addresses outside any resource
(`shadow.rs:230-247`); rectangle lists via a geometry shader (`primitive.rs`).

World: page pools (device 74/75, queue 31 CopyPageMappingsBatchX,
`resource.rs:2190-2230`, `queue.rs:897-948`); mesh draws (list 129/100,
indirect mesh dispatch, depth bounds); ExecuteIndirect with count buffers and
argument clamps (`indirect.rs`); GPU memory placement between 4 GiB and 2^40
(`VH\xmem.rs:73-87`).

## Step 3: port order inside the layer

1. Console ABI slot tables as data, with the Device11 layout switch (device,
   queue, list, fence). Test with `L\test\xs_proxy_mock_test.cpp`.
2. Synchronization: fence value words, Write32/64 labels, WaitValue holds
   (`L\src\queue_holds.h`), ExecuteX/Signal2X, timestamps, release at PresentX.
3. Pipelines built at first bind on worker threads with a per-frame wait
   budget; draws without a ready pipeline dropped; a list of pipelines earlier
   runs used, started early.
4. Title memory: a GPU address is the title's CPU address. Import xmem graphics
   memory with `VK_EXT_external_memory_host` (Leonida's `create_host_buffer`,
   `d3d12_vulkan_device.cpp:1974-2075`) as the default; keep copies only for
   memory that cannot be imported. Port xmem's address placement.
5. Descriptors (sampler stride 16, CreateSamplerX, views over title memory,
   slot lifetimes), then textures (upload queue, partial residency, detiling via
   `L\src\resource_detile.h`), then indirect (clamps, mesh indirect, counts),
   rectangle lists and frame pacing.

## Shaders: rules to take from VIctory into `tools/rdna2spirv.py`

`VS\` = `victory\crates\victory-shader\src\`. Some VIctory fixes exist only in
its history (commit `570a941e9` replaced the whole shader crate): `060195536`,
`2286e038c`, `77053a0c5`.

In order, each with a `tests/semantic` fixture (the suite needs no LLVM, SPIR-V
tools or GPU):

1. **Loop iteration cap** (stops GPU hangs): 128 pixel, 256 vertex/mesh, 4096
   compute, then discard/return (`VS\emit\lower.rs:328-335,11124-11173`,
   `15b0091e8`). Loops over unmirrored data that reads as zeros ran ~4 billion
   times and lost the card. Leonida has no cap; its loops follow the whole wave,
   so one counter per wave, and an early-exiting wave must not stall barrier
   rounds.
2. **Rectangle lists** (topology 19) through a generated geometry shader
   (`VS\emit\rect.rs:1-27`, `683ad7a99`); Leonida refuses the topology
   (`runtime\shims\d3d12_vulkan_device.cpp:3712-3718`). Full-screen passes and
   tiled lighting use it.
3. **Literal sampler compare**: the console writes `DEPTH_COMPARE_FUNC =
   LESS_EQUAL` into every static S#, so whether a sample compares is decided by
   the instruction (`VS\emit\sampler.rs:13-15`, `2286e038c`). Leonida enables
   compare from the S# field (`runtime\src\gfx10_descriptors.h:534,609`), so a
   plain `image_sample` gets a compare sampler: undefined behaviour. Key literal
   samplers by (words, compare).
4. **Quad ops and helper lanes**: quad-confined DPP and `ds_swizzle` quad mode
   as quad operations in pixel and mesh shaders (`lower.rs:4335-4370,
   4464-4495, 6671-6760`, `921d8d3b4`); a helper lane's `v_readfirstlane`
   returns its own value (`060195536`: waterfalls with only helpers left spun
   until DEVICE_HUNG ~200 s into the world); a discarded lane stays a helper
   (`77053a0c5`).
5. **VS → PS linking by signature row**, not parameter number
   (`VS\native.rs:37-48,203-241`, `lower.rs:1090-1251`); run a census of the
   corpus first.
6. **Per-program wave64 emulation** for speed on 32-lane GPUs: run Leonida's
   partition-invariance tiers (`tools\wave64_halves.py`) on compute and mesh
   too, emulate only tier C, add VIctory's exact idioms (`lower.rs:483-533,
   996-1017`, `VS\emit\own_bits.rs`).
7. Compute implicit derivatives (both use LOD 0 today), cube `_d` LOD pin,
   waterfall collapse (measure first).

Leonida is ahead on BVH node numerics, f16 rounding and named-level gathers.

Runtime shader lookup (`runtime\src\shader_library.h`): key entries on the
parts the translator reads (ISG1/OSG1/PSG1/PSV0) with an exact match first;
write the translator commit and flag profile into `index.json` and refuse a
mismatch; on a miss, log the hashes, dump the container and let `make_game.py`
translate it.

## Frame time: where Leonida can beat VIctory

VIctory's world frames cost 240-970 ms, mostly host copies of title memory
(2,655 MB compared and 95 ms of waiting per frame in City), every submission
waited for, and 391 MB/frame of texture staging while streaming.

- Import title GPU memory once (`VK_EXT_external_memory_host`): no refresh
  copies, write watch, snapshots, compares or copy-back. Commit pages before
  import, never import write-combined pages, free an import only after the GPU
  retired it, keep imports few and large (Windows drivers cap allocations at
  4,096).
- Buffer device addresses: a per-64 KiB page directory of device addresses (or
  one sparse 2^40 mirror buffer if imported memory can be sparse-bound) instead
  of `heap_base + va`; removes VIctory's 1,023-slot directory limit.
- Resizable BAR (on, on the test machine): hot read-mostly buffers written
  straight into VRAM.
- Asynchronous submission on timeline semaphores once copy-back is gone; real
  occlusion queries; GPU detiling or `VK_EXT_host_image_copy` for textures.
