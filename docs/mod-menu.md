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
