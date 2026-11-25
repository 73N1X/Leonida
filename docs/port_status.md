# Port status: Leonida + VIctory merge

This is the working log of the merged port. Leonida is the base repository. The
DexNom/VIctory runtime (Rust, Direct3D 12) is merged in as a git subtree under
`victory/` because it already runs the title past boot. Leonida's own runtime
(`runtime/`, D3D12 → Vulkan) does not boot the title yet.

Nothing from the game is in this repository. All runs read the user's own
package in place, and every derived file (shader cache, saves, captures,
logs) lives in the work folder outside the repository (`G:\gta6port` on the
test machine).

## Layout

| Path | What |
|---|---|
| `victory/` | DexNom/VIctory, subtree with full history. Updated with `python -I scripts/sync_victory.py --merge` |
| `scripts/sync_victory.py` | Fetches `victory/main`, reports new commits, merges them with `git subtree pull` |
| `scripts/port_ctl.py` | Starts a run, presses stand-in pad keys from a file, saves frames on request, reads the fps lines, stops the run |
| `runtime/`, `tools/`, `make_game.py` | Leonida's own runtime, shader translator and asset tools, unchanged |

## Test machine

Windows 11 Pro 26200, NVIDIA GeForce RTX 4060 Ti 16 GB (driver 616.64), AMD
Ryzen 5 7600X (6 cores, 12 threads), 31 GB RAM. Package: Xbox Series, in
`Documents\GTA6`.

## Build and run

```powershell
cd victory
cargo build --locked --release -p victory --features diagnostics --bins
$env:VICTORY_GAME_DIR = "$HOME\Documents\GTA6"; $env:VICTORY_CACHE_DIR = "G:\gta6port\cache"
.\target\release\victory.exe warm          # translate every shader ahead of time
cd ..
python -I scripts/port_ctl.py start --name run1
python -I scripts/port_ctl.py press Enter
python -I scripts/port_ctl.py shot
python -I scripts/port_ctl.py fps
python -I scripts/port_ctl.py stop
```

The BCPack format tables VIctory needs are not tracked. They are restored
from VIctory's own history (commit `2849477`, the one `Initialize-FormatTables.ps1`
pins) and checked against the pinned SHA-256:

```sh
git show 2849477415a46f5a079c85cf3e64a3dff32f713c:crates/victory-formats/src/bcpack-tables.bin > victory/crates/victory-formats/src/bcpack-tables.bin
```

## Changes on top of VIctory

- The stand-in pad command file (`VICTORY_MOVEMENT_FILE`) accepts every key of
  the stand-in pad's table, not only the stick directions, so menus can be
  driven from a script (`victory-hle/src/modules/gameinput/movement.rs`).

## Results

(Measurements are added here as runs are made.)

## Open items

1. Baseline run on the RTX 4060 Ti: boot, menus, first world frames, fps.
2. Find what bounds world frame time on this machine and cut it (target: 30 fps).
3. Black areas in shadow (ray-traced lighting does not run by default).
4. Hook the VIctory build into Leonida's `Build.bat` / `make_game.py`.
