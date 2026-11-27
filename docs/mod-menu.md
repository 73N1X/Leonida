# The trainer menu

Leonida's D3D12 runtime (`d3d12/`) has an in-game trainer menu, drawn by the runtime over the presented picture. It
is on by default; `LEONIDA_MENU=0` turns it off (no thread, no clock override, nothing drawn). Single player only:
nothing in it touches other players, money or anything online.

It is one of three things that runtime lays over the picture, each on its own key:

| Key | What | Code (`d3d12/crates/leonida-gpu/src/`) | Builds |
|---|---|---|---|
| F1 | the settings menu that came with the renderer merged at `00698e1e8`: launcher settings, saved to `leonida-settings.txt` | `host/menu.rs` | diagnostics |
| F3 | the numbers panel (frame rate, memory) | `host/panel.rs` | diagnostics |
| F4 | the trainer menu, this document | `host/trainer.rs`, and the crate `d3d12/crates/leonida-menu` | all |

Status: the menu was written against the renderer as it was at `db723a4c4` and brought onto the merged one
afterwards. It builds and its tests pass (below), the overlay's pipeline draws the menu on a D3D12 card offscreen,
but **it has not been tried in the running title yet**: where it lands on the real back buffer, how it looks with
frame generation, and whether the kept keys feel right there are not verified.

Code: `d3d12/crates/leonida-menu` (model, pages, input, run-time settings, CPU painter, a PNG encoder for the
previews) and `d3d12/crates/leonida-gpu/src/host/trainer.rs` (the overlay drawn at present, what the renderer lends
the menu, screenshots); `d3d12/crates/leonida-hle/src/modules/gameinput.rs` keeps the menu's keys from the title
and `modules/clock.rs` serves the title's clock.

Lua mods (`docs/lua-modding.md`) add pages to this menu: the last item of the main menu, Mods, has "Reload Lua mods"
and a page for each page a mod made. They are drawn and moved by this code (`leonida_menu::mods`), and a mod's
callbacks run on the mods' own thread, never on the menu's.

## Controls

| Key | Pad | Does |
|---|---|---|
| F4 | RB + d-pad Left | open / close |
| Up, Down or numpad 8, 2 | d-pad Up, Down | move (repeats while held, wraps round) |
| Left, Right or numpad 4, 6 | d-pad Left, Right | change a number or an option |
| Enter or numpad 5 | A | select: open a page, flip a toggle, run an action |
| Backspace or numpad 0 | B | back; on the first page, close |

The menu reads the keys only while one of the runtime's windows is in front. While it is open those keys (and the
pad's d-pad, A and B) are kept from the title (`gameinput.rs`), and so from the stand-in pad the keyboard drives
when no pad is connected; a key still held when the menu closes stays kept until it is let go, so the Enter or
Backspace that closed it does not reach the game. Closed, the title sees everything as before.

Beside the F1 menu: while the F1 menu is shown it has the keyboard (the title sees no key at all, as upstream has
it), and the trainer reads the pads only, so the arrows move one menu and not two; F4 is not read then either. The
two can be shown at once (F4 first, then F1): the F1 box is in the middle of the picture, the trainer on the left.

A thread of its own samples the keys and pads (every 16 ms closed, 8 ms open) and paints the menu on the CPU only
when it changes. A present asks one word what there is to do for the menu (`leonida_menu::work()`: a bit while the
menu is open, a bit while a screenshot is asked for), so a present with the menu closed pays one atomic load and
nothing else; the thread is started and the picture's height told when the presenter is made.

### Opening it without the keyboard

`LEONIDA_MENU_OPEN=1` starts the menu open on its first page, for a test that cannot press F4 (a window without the
keyboard focus). From the launcher: `scripts\play-d3d12.ps1 -Game <folder> -More @{LEONIDA_MENU_OPEN='1'}`. It
stays open until F4, Backspace or B closes it. The keys it keeps from the title are kept from the first present on;
the scripted keys of a test (`LEONIDA_KEYS`, `LEONIDA_KEYS_FILE`) are the title's and still reach it, and they do
not move the menu. `LEONIDA_MENU_OPEN` is the trainer's; the other `LEONIDA_MENU_*` variables (`_FILE`, `_PLAY`,
`_ARGS`, `_SMOOTH_MOTION`, `_CONSOLE`) belong to the F1 settings menu and its launcher.

## Look

The classic single-player trainer layout, drawn by Leonida's own code: a blue gradient banner with the title in a
script face, a black bar with the page's caption (light blue) and `n / N` (white), dark translucent rows with
white text, the selected row light with dark text, `>` on rows that open a page, on/off boxes for toggles,
`< value >` for numbers and options on the selected row, a bar with the scroll arrows and a box with the selected
item's help (amber when the item cannot work). 432 x 108 px banner, 34 px caption bar, 36 px rows, 11 rows at
once, 28 px from the top-left corner, all at a 1080-line picture and scaled with the picture's height (and with
Settings, Menu, Menu size). The F3 panel stands in the same corner: while it is shown the menu starts 8 px below
it. The highlight slides from row to row: two layers are painted (all rows unselected, all rows selected) and the
shader shows the second inside the band, so the band moves on the card without a new picture.

Where it is drawn: one blended quad onto the swap chain's buffer, after the title's picture is there (its two
planes laid over each other by `compose.rs`, or its one plane copied) and before `Present`, in the same list and at
the same point as the F3 panel and the F1 menu. With DLSS frame generation (`fg.rs`) the swap chain is Streamline's
and generated frames are made from what was presented, the three overlays included; no interface plane is tagged
for Streamline (upstream tags none either), so in generated frames the menu is treated like the rest of the
picture. Saved frames, the frame recorder and screenshots are read back before the overlays are drawn and show the
title's picture alone.

Fonts: the system's own, rasterised at run time by GDI (`GetGlyphOutlineW`): Segoe Script Bold for the banner,
Segoe UI and Segoe UI Semibold for the rows and the bar. Both ship with Windows 10 and 11; nothing of a font is in
the repository or the build. Off Windows (and in the model's tests) the built-in 5x7 dot font is used. No asset of
GTA V or GTA VI is used.

### Why not a port of the GTA V Native Trainer

Asked for: port the menu of the ScriptHookV SDK's NativeTrainer sample (or a "Native Mod Menu" project) if its
licence allows reuse. Found (2026-11-26):

- The NativeTrainer sample ships in Alexander Blade's ScriptHookV SDK from dev-c.com. No licence that grants reuse
  was found for the SDK or the sample; its source files carry only a copyright line. Without a licence the code
  cannot be copied into a GPL-3.0-or-later project, so none was.
- Enhanced Native Trainer (github.com/FIying-Scotsman/GTAV-EnhancedNativeTrainer), which grew from that sample, has
  a GPL-2.0 `LICENSE.md` with no "or any later version" notice found on the repository's page, and its menu is
  drawn with GTA V's own natives (rectangles and text through the game), so there is no portable layout code in it.
  Its provenance (built on the unlicensed sample) is a second reason not to take code from it.

So the look and behaviour were reproduced from scratch: proportions, colours, controls (F4, the number pad and the
arrows) and the grouping (Player, Vehicle, Weapons, World, Time, Weather, Teleport, Misc, plus Settings).

## What works

What is wired to something real, as built and unit-tested; none of it has been tried in the running title yet.

| Page, item | What it does | Where |
|---|---|---|
| Settings, Display, Sharpen strength | DLSS's sharpness, 0 to 1 in steps of 0.05: the same setting as the F1 menu's DLSS SHARPNESS (`LEONIDA_DLSS_SHARPNESS`, which `dlss.rs` reads at every upscale). There when the build has the `dlss` feature and `LEONIDA_UPSCALE=dlss`; dimmed otherwise. Whether the DLSS library in use still honours that parameter was not checked. (`LEONIDA_SHARPEN`, the menu's first target, went with the compose pass the merged renderer replaced.) | `knobs::SHARPNESS`, `host/trainer.rs` |
| Settings, Display, FPS panel | shows and hides the numbers panel (F3 does the same). Diagnostics builds; dimmed otherwise. | `knobs::PANEL`, `host/panel.rs` |
| Settings, Menu, Menu size | 0.6x to 1.6x | `knobs::MENU_SCALE` |
| Misc, Save screenshot | the next presented frame, without the menus or the panel, at full size, as a PNG in `LEONIDA_SCREENSHOTS` (else `screenshots` beside the runtime): 8-bit, 10-bit and half-float pictures. It is the present's own read-back (`capture::read_back`), so diagnostics builds; dimmed otherwise. | `host/trainer.rs`, `capture.rs` |
| Misc, Frame recording | every presented frame saved, by making and removing the file `rec` in the capture folder, which is what `capture.rs` looks for (a script that makes the file shows in the menu as on). Diagnostics builds with `LEONIDA_CAPTURE` set (the launcher's `-Capture`); dimmed otherwise. | `knobs::RECORD`, `capture.rs` |
| Misc, Frame-rate cap | Game, 30, 40, 60, 90, 120, 144, None: the renderer's own cap, the same setting as the F1 menu's FRAME RATE CAP (`LEONIDA_FPS_CAP`, the frame clock of `frame.rs`). Game is the title's own pace, None as fast as it renders; a rate set elsewhere that is not offered shows as the nearest. | `knobs::FPS_CAP`, `frame.rs` |
| World, Game speed / Slow motion | the rate of the title's `QueryPerformanceCounter` (0.1x to 2x; slow motion: one third) | `leonida-hle/src/modules/clock.rs` |
| Settings, About | the controls | |

The cap, the sharpness, the panel and the recorder are the renderer's own settings, lent to the menu
(`knobs::Owned`). The first three are in the F1 menu too: the two menus change the same thing, and each shows what
the other set (the F1 menu reads its live settings again when it opens; the trainer looks twice a second while
open). The F1 menu saves to `leonida-settings.txt`; what the trainer changes lasts for the run unless the F1 menu
is opened afterwards and saves it.

Game speed is experimental and not verified in game: the runtime serves the title's `QueryPerformanceCounter`
import (from `api-ms-win-core-profile-l1-1-0.dll`) and scales it continuously, so the title's time never jumps.
The executable calls that import from 273 places (counted as `call`/`mov` through its import slot), but it also
reads the processor's time-stamp counter directly 1,037 times (`rdtsc`, `0F 31`, counted over `.text`), which
no import reaches; what the title times that way keeps its pace, and which of the two its game clock uses was not
established. Until the rate is first changed, the host's count is passed through unchanged.

## What is not there yet, and why

Everything that needs the title's own scripts: god mode, wanted level, stamina, fast run and swim, super jump,
invisibility, ragdoll, every vehicle and weapon item, moon gravity, traffic, pedestrians, police, blackout, time
of day, weather, teleports, HUD and radio. These items go through `GameActions` (`actions.rs`); the backend there
today, `NotYet`, answers every one with the reason, and the menu shows them dimmed with that reason in the help
box. A backend that can call the title's script commands only has to implement `GameActions`.

### Teleport by memory

The Teleport page and `leonida.teleport(x, y, z [, hold_seconds])` do not use the script natives: they write the
player's position into the title's memory (`apps/leonida/src/player.rs`). The player's transform is a float
matrix (three unit axes and a position row; a 4x4, a 3x4 or a packed 4x3) in the title's writable memory, and a
physics copy of it moves with it, so every transform that moves with the player is shifted by the same amount and
written again every 5 ms for a few seconds (`hold`), so that the physics does not put the player back and it does not
fall through ground that is not loaded yet. The menu items work only when a position is known:

1. **Find it once** (diagnostics build): `LEONIDA_FIND_PLAYER=<start s>,<seconds>` (`player_find.rs`) lists every
   candidate transform at `<start>`, samples their positions every 250 ms while the test script's keys
   (`LEONIDA_KEYS_FILE`) are held, and ranks the ones that stay put when no key is held and move with W/S/D and Shift.
   `LEONIDA_FIND_PLAYER` alone means `90,240`. Besides transforms (strict forms only, so props with an
   identity-like matrix drop out) it tracks position vectors (a float4 whose w is a flag word, or three
   doubles) in the map's bounds, scans on several threads, and drops candidates that never move while keys are held.
   The best candidate is often a copy the game rewrites, so a write probe then tries every triple near it (x and y
   within 0.5 m, or off by a multiple of a 512 to 4096 m sector; z from 0.5 m below to 2.5 m above, f64 1 m below to
   3 m above), 32 at a time (+3 m for 300 ms, halving a batch that has an effect). The one that moves the copy,
   or that moves other copies, is what the teleport writes. A `LEONIDA_TELEPORT` waits for the probe.
   Then it searches for pointers to the best ones from the title's image (up to `LEONIDA_PLAYER_LEVELS` = 3 levels).
   Everything is written under `%TEMP%\leonida-player\`: `summary.txt`, `candidates.txt`, `tracks.csv`,
   `comovers.txt`, `best.txt`, `pointers.txt` and `paths.txt`. The run that found them can teleport at once.
2. **Use it later**: `paths.txt` (or `LEONIDA_PLAYER_PATHS=<file>`) is read at start-up; a path is a pointer
   chain from a writable section of the image (an RVA, so it holds for the same build only).
3. `LEONIDA_TELEPORT=<delay s>,x,y,z[,hold ms]` teleports once; `LEONIDA_TELEPORT_LIFT=<m>` is added to z;
   `LEONIDA_VICE_CITY=x,y,z` (or `vice.txt` beside the paths) is the target of "To Vice City". Without a known position
   the items keep saying they are not available.

None of the addresses or paths is committed; they are the user's own run's.

### Looking for the script natives

A trainer for a RAGE title calls the script engine's natives: GTA V's registers each command's handler under a
64-bit hash at start-up, and trainers find the table, or the registration calls, by pattern. This was looked for in
the user's own `game_scarlett_master.exe` (SHA-256 `e95020b2...5a08993`, 120,181,760 bytes; read locally, nothing
of it copied). The image is a plain PE32+ (base `0x140000000`, `.text` `0x5F3F966` bytes, `.rdata` `0x8A0C0C`,
`.data` raw `0x622E00`); no key or decryption is involved in reading it. Counted over the whole image:

| Looked for | Found |
|---|---|
| Names of GTA V natives as text (`PLAYER_PED_ID`, `GET_ENTITY_COORDS`, `SET_ENTITY_COORDS`, `GET_HASH_KEY`, `SET_TIME_SCALE`) and script-engine words (`scrThread`, `scrProgram`, `native`, `.ysc`) | none |
| GTA V-style registration: `mov r64, imm64` (a hash) and `lea r, [rip+x]` (a handler) before a `call`, grouped by the callee | the most-called such callee has 77 call sites; a table of thousands of natives would give one callee thousands |
| Dense blocks of 64-bit immediates that look like hashes (not pointers, not sign-extended), per 16 KiB of `.text` | at most 179 in any block; a registration function would hold some 800 per block |
| Tables of (hash, handler) pairs of qwords in `.rdata` and `.data`, either order, runs of 40 or more | none |
| Runs of 300 or more qword pointers into `.text` in `.rdata` | 1,769 runs, but each has few distinct targets (e.g. 1,261 entries, 136 distinct): vtables, not a table of thousands of distinct handlers |
| Runs of 1,000 or more 32-bit `.text` RVAs | none |
| 32-bit hash registration: `lea rcx, [rip+handler]; mov edx, imm32; call` | one callee with 554 such call sites in a 9.8 KiB function, handlers in `.text`; 4 of its 554 hashes match the one-at-a-time hash of strings in the image (`Water`, `TERRAIN`, `PED`, `decoration`), so it looks like a registry of types by name, not of script commands; too few for GTA V's thousands of natives in any case |

The scripts themselves are no help yet: `script.rpf` and `story000_script.rpf` hold their data in the framing and
proprietary coder `docs/coder.md` describes, so the scripts' native tables (the hashes they call) cannot be read to
match against the executable.

Conclusion: in this build the natives are not registered in any form these searches find (not as GTA V does it, and
not as a plain table). They may be registered from data built at run time or kept in a form that is not plain;
following that would need either a run-time trace of the script VM (finding where a script thread dispatches a
command) or the scripts decoded first. If it turns out to be obfuscated on purpose, the rules say to stop and write
it down here, which is where this stands.

### Looking for the tables at run time (stage 1)

The static searches above read the file; the title builds its script tables in memory. A diagnostics build
(`--features diagnostics`) can look there: `LEONIDA_NATIVE_SCAN=<seconds>[,<seconds>...]` starts a thread that, that
many seconds after the title starts, walks the process's committed memory (`VirtualQuery`; read through
`ReadProcessMemory` on the own process, so a page that vanishes fails the read instead of the process; nothing is
written to the title). Its own copy buffer is a separate allocation that the walk skips; the runtime's other
allocations cannot be told apart, so the output tags each region (`heap-rw`, `title-image-rw`, ...) instead.

It looks for runs of at least `LEONIDA_NATIVE_MIN_RUN` (64) elements that all point into an executable section of
the title: qwords with a stride of 1 to 8 qwords (an array, `(hash, handler)` pairs, wider buckets; any slot may
hold the handler) and 32-bit values that are RVAs into code. A stride-1 run hides the longer strides inside it. Runs
with at least `LEONIDA_NATIVE_MIN_DISTINCT` (200) distinct handlers are listed (the 200 best), with their overlap,
the share of the union of handlers each covers and, for strided runs, per slot how many distinct values and how
many look like a 64-bit hash. `LEONIDA_NATIVE_ALL=1` also scans read-only and DLL-backed memory.

Stage 1 (a first run, 100 s into the world) was noise: with every integer below the image's code size counted as an
RVA, the union was 2.5 million "handlers" over every MiB of code. Stage 2 makes the scan specific:

- A pointer (or an offset) only counts when its target is a function start: a `.pdata` function begin (299,257 of
  them in the title's `.pdata`), or a 16-aligned byte after `int3` padding (a leaf function has no unwind entry).
  `LEONIDA_NATIVE_LOOSE=1` drops this. 99.6 % of the `.pdata` functions are 16-aligned.
- 32-bit values are off unless `LEONIDA_NATIVE_U32=1`. Then it looks for arrays of offsets from an unknown base:
  handlers are 16-aligned, so offsets from one base share their low four bits; a run of at least 12 non-zero values
  with the same low nibble is checked against the function starts by trying every function start as the target of
  one of its first two values, and kept when at least 90 % of the run lands on function starts. This finds the base
  (the global the interpreter adds the offset to) without finding the interpreter, and base 0 would be an RVA table.
  Per base, the summary lists the arrays, the distinct handlers and the qword slots of the title's writable sections
  that hold the base as an address (the global itself).
- Defaults are now a run of 32 and 64 distinct handlers (`LEONIDA_NATIVE_MIN_RUN`, `LEONIDA_NATIVE_MIN_DISTINCT`).

Output, kept out of the repository because it holds addresses of the title: `%TEMP%\leonida-natives\scan-<s>s\`
with `summary.txt` (memory scanned, function starts, runs by distinct count, the union and its RVA histogram per
MiB, the bases), `candidates.txt`, `targets.txt` (handler RVAs per candidate), `union.txt`, and in 32-bit mode
`offset-arrays.txt` and `offset-handlers.txt` (the handlers per base). Results of a real run go here next, as counts
and method only.

Finding the interpreter statically (to read which global it adds the offset to) was tried on the file and did not
get anywhere, counted over `.text`: 10,789 `jmp reg`, 2,759 of them after a `cmp` and bounds check with a
`lea`-addressed table; only about 20 tables have 200 or more cases, and those are id-to-id maps (`mov eax, imm; ret`
cases), an entity-type switch and parsers on a byte with at most 97 cases; no function of the 161 over 12 KB has a
dispatch on an opcode byte of 150 or more cases, and no `add reg, [rip+global]` before a `call reg` sits in a large
function (the 14 such globals have 1 to 3 sites each, all in small functions). Nothing was de-obfuscated; the
interpreter is simply not found by these signatures, which is why the run-time search above does not need it.

## Tests

- `cargo test -p leonida-menu`: the model (navigation, wrap-around, scrolling, the back stack, toggles, numbers,
  choices, actions, unavailable items), key repeat and debounce, which keys are kept from the title, the clock's
  continuity, the pages, settings lent by an owner, the one word a present asks, `LEONIDA_MENU_OPEN`, the painter's
  layout, the PNG encoder, and GDI glyphs (Windows). No GPU.
- `cargo test -p leonida-gpu --features diagnostics --lib host::trainer`: draws the menu with the overlay's own
  pipeline onto an offscreen D3D12 texture over a made-up backdrop, at its place and below where the panel would
  be, reads both back, checks the banner, rows and backdrop, and saves a PNG (`LEONIDA_MENU_PREVIEW=<path>`, else
  `leonida-menu-preview.png` in the temporary folder); and how the cap and the sharpness are read and written.
  Skipped without a D3D12 device.
- `cargo test -p leonida-hle --features diagnostics --lib -- gameinput clock`: the input shim with the kept keys in
  place, and the title's clock.
- `cargo run -p leonida-menu --example preview -- <out.png> [height] [main|player|display]`: the same picture
  painted on the CPU alone.

None of these runs the title. What only a run can show is listed under Status at the top.
