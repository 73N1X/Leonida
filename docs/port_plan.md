# Port plan: what to take from VIctory into Leonida, in order

Leonida's runtime is the base; VIctory (`victory/`) is the reference that has
run the title. This plan lists what the title needs that Leonida's runtime does
not do yet, in the order the title hits it, and where each fix lands.
Path roots: `L\` = `runtime\`, `V\` = `victory\crates\victory-gpu\src\`,
`VH\` = `victory\crates\victory-hle\src\modules\`.

## Measured so far (`gta6rt` on the 1.0.2.1 package)

- `--dry-run`, `--init`: all five modules map and initialise (TLS callbacks,
  DLL entry points). 11 imports are unimplemented:
  `XCRDAPI.dll!DStorageGetFactory`, `xapu.dll!XApu{Connect,DequeueResult,
  Disconnect,EnqueueCommand,GetResultCount}`, `chad.dll!XDsp{Activate,
  ConnectWithMaximumStreamLimit,Deactivate,Disconnect,SubmitCommand}`.
- `--run`: the GDK runtime initialises offline, then the title calls
  `DStorageGetFactory` and stops (about 3 s in).

## Step 1: system calls the title needs before its first frame

| What | VIctory source | Lands in |
|---|---|---|
| DirectStorage (factory, queues, requests, status arrays, zlib, BCPack) | `VH\dstorage.rs` | new `L\shims\dstorage_xs.cpp`; inflate from `L\src\shader_library.h` |
| BCPack decode (tables supplied locally, SHA-256 checked, never committed) | `victory\crates\victory-formats\src\bcpack*` | new `L\src\bcpack.h/.cpp` |
| APU (`xapu.dll`) and DSP (`chad.dll`) | `VH\apu.rs`, `VH\dsp.rs`, `VH\audio\` | new shims next to `L\shims\acphal.cpp` |

## Step 2: the console D3D12 ABI in front of the Vulkan layer

The title creates its device as `ID3D12Device11`, which selects the newer
console layouts: list slots from 59 and queue slots from 14 are two slots later
(`V\device.rs:149-168`, `V\list.rs:2134`, `V\queue.rs:985`). The proxies now
take every slot's meaning from the slot tables in `L\shims\d3d12_xs_slots.h`
(device, queue, list, fence; each entry cites its source), with that switch: a
device created as `ID3D12Device11` gets the console layouts, any other keeps the
PC order Leonida's own PC-interface tests use. `L\test\xs_proxy_mock_test.cpp`
checks every queue and list slot of the tables through the proxies.

Boot, in order:

| # | Title calls (VIctory reference) | Leonida now |
|---|---|---|
| 1 | Queue Signal 16, Wait 17, GetTimestampFrequency 18 (`queue.rs:960-987`) | PC 14/15/16; GetDesc 28 → PC 18, the return buffer first or second |
| 2 | Fence slot 12: address of the 64-bit value the title polls (`fence.rs:21-27,136-139`) | a word the proxy keeps at the fence's completed value (CPU signals at once, queue signals through one completion event per signal) |
| 3 | Device 69 (debug filter, no-op), 127 CreateCommittedResource3 for back buffers (`device.rs:442,457`; `resource.rs:2162-2186`) | 69 S_OK; 127 → PC CreateCommittedResource3 (PC description, ALLOW_DISPLAY cleared), else CreateCommittedResource |
| 4 | Queue 35 ExecuteX, 34 Signal2X, 32 PresentX (`queue.rs:597-610,757-841`) | 35 → PC 10, 34 → PC 14; 32 → `set_present_handler`, else S_OK with nothing shown. `XboxPresentation` is not attached: gta6rt has no window yet, and its FramePacer hands out its own tokens and models only the origin event |
| 5 | Device 78-81 frame clock (`frame.rs:1-130`) | VIctory's clock per device (S_FALSE time-outs, token on time-out, flips on consecutive ticks) |
| 6 | Lists from device slot 12 get the console layout (`list.rs:566`) | console list proxies (also as the console's List5/List6 IIDs); device 25/26/108 take the return buffer first; device 48 takes the console queue description |
| 7 | List 139 ResetX, 80 DispatchX, 71/72 Write32/64, 73/74 WaitValue, 62 CopyMemory, 86 CopyResourceX, 148 AllocateUploadMemory (`list.rs:2059-2149`) | 139 → PC Reset, 80 → Dispatch, 86 → CopyResource; 71-74, 62 and 148 still return 0 (no Vulkan-layer implementation for title addresses) |
| 8 | Queries at slots 28-30 (`list.rs:2114-2117,1330-1345`) | → PC 52-54; 59 is the console marker (ignored); 27, 31, 56-58 and 60 are named by no source and refused |
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
