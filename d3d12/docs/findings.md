# What was learned on the RX 7900 XTX (2026-11-23 to 2026-11-25)

One PC: i9-12900K (8 fast cores, 8 slow ones, 24 threads), 64 GB, Radeon RX 7900 XTX
24 GB, Windows 11, 1440p, Series X profile. Figures are of diagnostics builds with a
screenshot a second. Scenes are not the same from run to run, so read differences
under about 5 % as nothing. What a switch does is in the comment where it is read.

## Where a frame goes

Median of whole runs, same part of the prologue: 280 ms, then 124, 103, 75, now 46-65.
On the water (first boat mission) 46-53 ms.

The title needs 33 ms. Of a 55 ms frame on the water:

- **The thread that prepares submissions: 48 ms.** It is the wall. Steps of the lists
  18 (refreshes of host copies 10), steps done on the card 7, waiting for descriptor
  records 6, asking the write watch 5, copies in the card's memory 4, the title's bytes
  under shader writes 4.
- **The card: about 41 ms busy**, the next wall.
- **List calls: 36,000 a frame, 230 ms of work** spread over eight threads. In a street
  chase 67,000 calls and 430 ms.
- Descriptor records copied as plain bytes: 400,000 slots looked at, 40,000 mirrored a
  frame, most of them empty.
- Landing what shaders wrote: 16 MB a frame put as dwords, on a thread of its own.

## The one idea behind most of it

On the console the CPU and the graphics processor share memory. Here they do not, so
everything one writes and the other reads must be noticed and moved. Every large cost
is one of four: **noticing** writes of the title (write watch, guards), **noticing**
writes of shaders (hashes on the card, compares on the CPU), **moving** them, and
**waiting** until an order is safe. Gains came from making each of them narrower
(dwords, not pages), wider (threads, AVX2) or later (on the card, behind the frame),
in that order of size:

| What | Frame before | after |
|---|---|---|
| Descriptor records of draws' tables left to the draws | 280 ms | about 124 |
| Our hash passes over card copies, 64 threads a piece | card 79 ms | 66 |
| Preparing on a thread of its own, refreshes and records spread | 103 | 75 |
| List calls written down and translated by eight threads (`LEONIDA_THIN`) | 75 | 68 |
| Submissions joined, card steps, textures sent ahead | 68 | about 63 |
| Title bytes under shader writes compared with AVX2 on six threads | 14 ms of it | 4-7 |
| Stores into guarded memory looked at first, fewer guards reset | 69 ms of threads' time | about 6 |

(Each figure is from the run in which it was measured; they overlap and do not add up.)

## Faults found, and what they were

- **Boats sank, the sea plane went under.** The ocean's height and displacement maps are
  textures that compute passes write and the title's physics sample on the CPU. They were
  never brought back to title memory. Fixed by Geckomacabre's read-back as merged in
  fxceless/victory-fork f5c03f3b; on by default here (`VICTORY_TEXTURE_BACK=0`: off).
  With it the boat stays up through a whole run; without it sank in every one.
- **A device loss about every 16 minutes**: a page fault inside a texture made in the same
  tenth of a second. A slot showed it before its upload had run.
  `LEONIDA_SHOW_AFTER=1`; none in the sessions since (likely fixed, not proven).
- **A rare lost write** (6 of 200 runs of a stress test): a view was mapped between our
  look at its guards and our lock, and views were opened one after another. None in 603
  runs after.
- **Blocky patches**: indirect dispatches cut at 65,535 groups.
- **Square holes and dashed edges in shadows**: pixel shaders that read their quad's
  neighbours with wave reads; on AMD helper lanes take no part in them.
- **Cube reflections too blurred, wrong materials**: DXIL's `Log` is base 2, and
  normalized buffer codes were one step off.
- **Every ray missed**: the dispatch flag that says counts are threads, not groups.

## Measured and not worth it

- Unordered early pipelines for everything (AMD's compiler crashed).
- Series S profile, smaller textures, a second drawing queue, `ASYNC_SUBMIT` off.
- Mapping pages on sixteen threads: no more pages mapped, frames longer.
- Scalar buffer loads four dwords a call (`LEONIDA_SHADER_OPT=511`): no gain on this card.
- Wave sizes, fused multiply-adds, fast reciprocals, raw picked buffers, leaving out
  barriers: nothing to 0.1 ms each. Exact RDNA float rules (`LEONIDA_RDNA_FLOAT`): 0.8 ms.
- Running the console's shader code natively on the card: the Windows driver takes no raw
  code, the two generations differ, and a Vulkan host layer ends at the same compiler. What
  is left to gain from shader code was guessed at under 1 ms.
- An on-screen panel of numbers (`LEONIDA_PANEL`): it helps finding where a frame goes but
  costs some, so it is off and F3 shows it.

## Translated compute programs on an RTX 4060 Ti (2026-11-27)

Counted offline over the 2,703 compute programs of the title that translate (the
`translate_cdxil` example: `--census`, the NVIDIA host settings, every translation also
through `dxil.dll`'s validator). The card time of a frame barely moves between 1080p and
1440p, so what counts is per group and per command, not per pixel. What the clean-up and the
wave64 forms took out, as instructions of the written module and group barriers:

- Exchanges of a console wave64 program in two host waves of 32 that run once
  (`LEONIDA_EXCHANGE_SITES`, dwords of their own, one barrier): barriers 10,643 -> 9,156.
- A first-lane read of a load from a read-only view at a lane-uniform address needs no exchange
  (`LEONIDA_UNIFORM_LOADS`): 9,156 -> 9,108.
- `LEONIDA_SHADER_OPT` bit 128 (a value kept for the lanes a guard leaves out, read only where
  the guard holds, is its new value): guard selects 488,284 -> 281,592, instructions -1.5 %.
- `LEONIDA_SHADER_OPT` bit 256 (a conjunction of lane conditions with its terms in the order
  they were made, so the enclosing branches' share is one value).

What is left, by size: exchanges inside loops (two barriers each: about 110 of the 134 barriers
of the 8 x 8 material program `1f1fbd86759b3da2` are in loops), the entry ticket of every
console wave64 program with lane code (a group barrier and an LDS atomic, also for groups of
one dimension, where the host's waves are runs of consecutive ids), descriptors made again in
every branch of a program (`createHandleFromHeap` of an index that is a root constant plus an
offset, behind a branch on a load), and reads of read-only views repeated in blocks that
follow one another (70 of 118 in `922e1d0ee1e19a85`).

## Group packing (`LEONIDA_GROUP_PACK`, off by default)

The card's time barely moves between 1080p and 1440p and is far above what the instructions of
its programs cost (`ffdb43445541c981`: about 0.08 ms of arithmetic, 2.9 ms measured), so the
suspect is launch and occupancy: 1,558 of the 2,703 compute programs use groups of 32 threads or
fewer (1,460 of exactly 32), some with 57,600 groups to a dispatch, and an Ada multiprocessor
holds 24 groups, which are 24 of its 48 warps when a group is one.

`LEONIDA_GROUP_PACK=<K>` (2, 4 or 8; the largest of them not above the number; 0 or unset: off)
translates a program of one console wave of 32 lanes with no group barrier to a host group of
`K` such waves (`numthreads(32 * K, 1, 1)`):

- host thread `t` is console thread `t % 32` of slot `t / 32`; the console thread's x, y, z are
  taken from the console group's shape; each slot is a host wave of its own (a one-dimensional
  group on a host of 32-lane waves: `lower::Pack`);
- the console group number is `(x + 65535 * y) * K + slot` of the host group (X, Y), taken apart by
  the console's grid, which the host puts in three root constants (`USER_PACK_X`, `USER_PACK_Y`,
  `USER_PACK_GROUPS` of `shader.rs`; their metadata `dword` is `K`, so a cached translation says
  it is packed). A thread whose number is not below the groups in all leaves at once, which is
  right because there is no barrier;
- group-shared memory is a slice for each slot (`slot * L` dwords added to every LDS address,
  `K * L` dwords in all, at most D3D12's 8,192); `K` comes down until it fits, and until
  `K * 32 * vgprs` is 32 K registers or less;
- the host runs `pack_grid(groups, K)`: the console's groups in all divided by `K`, rounded up,
  as `(min(n, 65535), ceil(n / 65535), 1)`. Direct dispatches do it in `State::dispatch`; indirect
  ones in the clamp (`indirect::bound_packed`, the batched clamp's job word 3), which writes the
  console's grid and the host's dispatch as one 24-byte record, executed with a command
  signature of three root constants and a dispatch (`draw::pack_signature`).

Not packed, with the counts over the 2,703 (`translate_cdxil --census` prints `pack K` or `pack 0
<why>`): 1,237 groups of other than 32 threads, 41 dispatches counted in threads (the structure
builder's), 28 console wave64 programs. **1,396 are packed** with `LEONIDA_GROUP_PACK=4` (1,395 at
4, one at 2 for its group-shared memory): 1,229 of 32 x 1 x 1, 146 of 8 x 4, 15 of 4 x 8, 5 of
4 x 4 x 2, one of 2 x 2 x 8; 77 use group-shared memory, 554 use over 128 registers. With `LEONIDA_GROUP_PACK=8`: 837 at 8, 558 at 4
(the registers), one at 2, none refused by the validator. The programs
`ffdb43445541c981`, `cb6adcc6da2613ff`, `5cf00dc39042edaf` and the tile lists'
`2dc364df5d3278b8` and `940c2635dc1312f4` are among them. All of the packed translations pass
`dxil.dll`'s validator.

What it does not do, and what to look at: a program with a group barrier is not packed (the
slices would have to meet at it, and the tail of the grid would have to wait there); groups of
fewer than 32 threads are not (a wave would hold several groups); wave64 programs are not.
Shaders with lane operations are packed (each host wave is one console group where the host has
waves of 32 lanes, as it does here): that rests on a 1-D host group giving a wave a run of 32
consecutive thread ids, which `LEONIDA_GROUP_PACK=0` takes away again if a program misbehaves.
The translator already takes a console group-shared memory address as in bounds (a shortcut);
packed, an address past the group's memory reaches the next slot's instead of the host's end.

## What is next

1. The preparing thread: split what one submission does over two threads in a row; ask
   for the next submission while this one is sent; view the card's own buffers where fills
   copy them now.
2. The card: small buffers' copies in its own memory (`LEONIDA_CARD_BUFFERS_MIN_KB=64`,
   after fxceless c36971b2; not measured here yet), and the passes that run for nothing
   without rays.
3. Stalls: 270-660 s of a session were full of frames over 200 ms, almost all while new
   pipelines were made. `leonida warm`, and a quarter of the workers for the slow ones.
4. Outdoor flicker: suspected to be one bad indirect record that uses up its list's
   budget, so that every later draw of it is empty. The log's frame line counts them;
   `LEONIDA_INDIRECT_REST=0` is the experiment.
5. Memory, 40-46 GB private: lists' allocators 9 GB, windows 7, uploads 4
   (`LEONIDA_MEMORY_MAP=1` lists it).

## The other repositories, compared

Read up to 2026-11-25: Marvisak/VIctory, fxceless/victory-fork, DexNom/VIctory,
Belobug/VIctory-Belobug-AMD, FQY55kai/ZJOU-VICTORY. What was taken is in the history,
each commit naming its source. In short:

- Marvisak's commits hardly apply any more; nearly all of what they fix is here, often
  another way. Taken: nearest normalized codes, d16 views, shader model flags.
- fxceless: the base. Newest taken: texture read-back, small buffers on the card, hashes
  over a buffer's own bytes.
- DexNom: thread counts of ray dispatches, cube gradients, joined sparse ranges.
- Belobug: pipeline workers for screen passes, the ambient stand-in's idea, pipeline and
  constant buffer bookkeeping (romainhedouin).
- ZJOU: render targets not set again; some shader corrections are being looked at.
