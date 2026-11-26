# Leonida on Linux (D3D12 runtime)

Leonida's D3D12 runtime is the VIctory build under `victory/` (since `bafb0e25b`, trustno1-coder/VIctory-NVIDIA). It is a
Windows program, so on Linux it runs under Wine. Its Direct3D 12 comes from **VKD3D-Proton**, which translates D3D12
to Vulkan. VKD3D-Proton ships inside the Linux package, so players download nothing else for D3D12.

## What a player needs

- Wine (the distribution's `wine` package, or Proton's), 64-bit.
- A Vulkan driver: Mesa (AMD, Intel) or NVIDIA's.
- Their own copy of the game.

## What the package holds

| Path | What |
| --- | --- |
| `victory.exe` | The Windows build of `victory/` (`cargo build --profile diag -p victory --features diagnostics`) |
| `play-linux.sh` | `scripts/play-linux.sh`: the launcher |
| `vkd3d-proton/d3d12.dll`, `d3d12core.dll` | VKD3D-Proton 3.1, 64-bit, unmodified |
| `vkd3d-proton/LICENSE`, `COPYING`, `SOURCE.txt` | Its LGPL-2.1 text, notice, and where its source is |

The package's `vkd3d-proton/` folder is made with:

```sh
python -I scripts/fetch_vkd3d_proton.py --out dist/linux/vkd3d-proton
```

It downloads the pinned release, checks every file against the SHA-256 in the script, and extracts the two 64-bit DLLs.
It needs Python 3.14 or newer, for `compression.zstd`. `dist/` is not committed.

## What the launcher does

```sh
./play-linux.sh /path/to/game_scarlett_master.exe [--series-x]
```

- **Wine prefix:** it uses Leonida's own (`LEONIDA_WINEPREFIX`, default `~/.local/share/leonida/prefix`) and makes it
  on the first run. No other prefix is touched.
- **D3D12:** it copies the bundled `d3d12.dll` and `d3d12core.dll` into that prefix's `system32` when they are
  missing or changed, and sets `WINEDLLOVERRIDES=d3d12,d3d12core=n` so Wine uses them.
- **Game files:** it points `VICTORY_GAME_DIR` at the game's folder. Caches and saves go to
  `~/.local/share/leonida/{cache,data}`.
- **Settings:** it uses the measured NVIDIA settings of `scripts/play-nvidia.ps1` as defaults; any `VICTORY_*` you set
  wins.
- **Start:** it runs `wine victory.exe run <exe> --execute --console series-s`, or `series-x` with `--series-x`.

On Windows nothing changes: the DLLs live only in the Linux package's `vkd3d-proton/` folder and never sit next to a
Windows `victory.exe`.

## Licences

VKD3D-Proton is LGPL-2.1-or-later and VIctory is GPL-3.0-or-later; shipping the unmodified DLLs beside the GPL program,
with VKD3D-Proton's license and a pointer to its source (`SOURCE.txt`), meets the LGPL's terms for distributing the
library.

## Not tested yet

- The launcher has been syntax-checked (`sh -n`) only, not run on a Linux machine.
- The Windows build under Wine has not been run either.
- Whether the D3D12 features the game needs (Shader Model 6.6, mesh shaders) all work through VKD3D-Proton 3.1 on a
  given driver is still to be measured.
