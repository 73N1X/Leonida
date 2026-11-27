# Button prompt glyphs

The title draws its button prompts from one Scaleform movie, and it has only the Xbox set: the dump holds no
PlayStation glyphs. This note records the movie's format and which export is which button, measured on the user's own
`data.rpf`. It holds no shapes. `tools/ps_prompt_glyphs.py` builds an original replacement set.

## The movie

| fact | value |
|---|---|
| where | `data.rpf`, a zlib stream at archive offset `0x117d7000` (found by zlib scanning; the TOC is not used) |
| size | 9,644 bytes stored, 28,148 bytes inflated (the inflated length is also the length field of the header) |
| signature | `GFX` (uncompressed Scaleform container), version byte 42 (`GFX*`), then the u32 file length |
| stage | 1 frame, 30 fps, frame label `platform_xbsx_input_icons_Main` |
| tags | GFx exporter info (1000, library name `input_icons`), FileAttributes (69: ActionScript 3), background colour, frame label, 131 DefineShape3, 38 DefineSprite, ExportAssets (56), DoABC (82, 9,516 bytes, lazy-init name `frame1`), SymbolClass (76, 39 entries), ShowFrame, End |
| symbols | 38 exports `platform_xbsx.input_icons.Main_Button_<N>` (N = 4..39, 58, 59), each a one-frame sprite of 2 to 5 vector shapes (no bitmaps); the SymbolClass binds each sprite to the class `platform_xbsx.input_icons:Main_Button_<N>` and one more entry, id 0, to the document class `Main` |
| canvas | every sprite is 1280 x 1280 twips (64 x 64 px), registration at the top left; exceptions: N = 16 and 25 (4..1281 x 21..1265), 34 and 36 (the bumpers, about 1279 x 775 starting at y 252) and 35 and 37 (the triggers, x 31..1254) |

The shared ActionScript builds the symbol name from `"platform_" + capabilities.toLowerCase()` and
`.input_icons.Main_Button_<iconID>`; the executable also holds the format string `platform_xbsx.input_icons.Main_Button_%d`.
Character ids of the original sprites follow the export order, 5, 10, 16, 20, ..., 169 (shapes in between); a replacement
does not need them, only the export names and the class names.

## N to button

Read from rendered sprites (our own rasteriser of the DefineShape3 records).

| N | button | N | button |
|---|---|---|---|
| 4 | D-pad up lit | 24 | right stick right |
| 5 | D-pad down lit | 25 | right stick press (R3) |
| 6 | D-pad left lit | 26 | right stick, plain |
| 7 | D-pad right lit | 27 | right stick up/down |
| 8 | D-pad, nothing lit | 28 | right stick left/right |
| 9 | D-pad, all lit | 29 | right stick rotate (arrow on the left) |
| 10 | D-pad up/down lit | 30 | A |
| 11 | D-pad left/right lit | 31 | B |
| 12 | left stick up | 32 | X |
| 13 | left stick down | 33 | Y |
| 14 | left stick left | 34 | LB |
| 15 | left stick right | 35 | LT |
| 16 | left stick press (L3) | 36 | RB |
| 17 | left stick, plain | 37 | RT |
| 18 | left stick up/down | 38 | Menu (three bars) |
| 19 | left stick left/right | 39 | View (two overlapping windows) |
| 20 | left stick rotate (arrow on the left) | 58 | left stick rotate (arrow on the right) |
| 21 | right stick up | 59 | right stick rotate (arrow on the right) |
| 22 | right stick down | | |
| 23 | right stick left | | |

Not identified: nothing is unknown by shape, but the Xbox/Guide button has no glyph in the set, and which of the two
rotate variants (20/29 against 58/59) the title picks for which prompt is not known. N 8 and 9 are told apart only
by the lighting; which one the title uses for a plain "D-pad" prompt is not measured.

## The PlayStation replacement

`tools/ps_prompt_glyphs.py` writes a movie with the same export names, canvas sizes and class names:

| Xbox N | replacement |
|---|---|
| 30, 31, 32, 33 (A, B, X, Y) | cross, circle, square, triangle in blue, red, pink, green |
| 34, 36 / 35, 37 | L1, R1 / L2, R2 |
| 38 / 39 | Options / touchpad (the console's View is the touchpad on the PlayStation layout) |
| 16, 25 | L3, R3 |
| 4..11 and 12..29, 58, 59 | the same d-pad and stick prompts, drawn again |

Shapes are discs, polygons, thick polylines and a small stroke font written in the tool. See the tool's docstring for
the options (`--out`, `--zlib`, `--pad-to`, `--selftest`). The movie is 13,551 bytes, 5,721 as one zlib stream, below the
original stored 9,644, so it can be padded to that size. The ActionScript block is minimal (empty `MovieClip`
subclasses, no `Main` behaviour, no sound hook); whether the title's loader needs more has not been tried in the game.
