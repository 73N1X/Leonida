# The Leonida Launcher

A desktop window for the port, in the usual shape of a game launcher: a narrow navigation column on the left, a hero
area with a big PLAY button, a status line and pages. It is Leonida's own design: no other company's name, logo, font
or artwork, and no game assets in the repository. The hero shows the newest screenshot you have taken (read at run
time from the runtime's `screenshots` folder next to `leonida.exe`, else `%LOCALAPPDATA%\Leonida\captures`), or a
generated gradient. It is not an official product.

## Starting it

- Windows: double-click `Launcher.bat` at the repository root (it finds Python 3.12 or newer, like `Mods.bat`).
- Linux: `python3 tools/leonida_launcher.py` (needs Tkinter: the `python3-tk` package).

Standard library only (`tools/leonida_launcher.py`).

## Pages

- **Play**: PLAY runs `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\play-d3d12.ps1 -Game <folder>
  -Data <folder>` plus the switches chosen in Settings (on Linux `scripts/play-linux.sh <folder>/game_scarlett_master.exe`,
  with `--series-x` if chosen). The script's output goes into the log box. The state line reads a few of the script's
  own messages: Starting, Building the runtime, Preparing shaders, Running, Exited with code N (a guess from the text;
  the log is the truth). Only one thing runs at a time: while it runs PLAY is replaced by Stop, which, after a
  confirmation, ends the process and everything it started (`taskkill /T /F`; on Linux the process group).
- **Settings**: the game folder (a picker, and a check that `game_scarlett_master.exe` is in it), the data folder
  (empty = the script's default), the mods folder, and the options of `play-d3d12.ps1`: DLSS (off is `-NoDlss`),
  `-RayTracing`, `-SmoothMotion`, shader preparation (off is `-NoPrecompile`), `-SeriesX`, `-Quality`, `-Resolution`
  (default = none), `-Capture`, `-Log`. On Linux only the game folder and Series X are used.
- **Mods**: the `mods\*\mod.json` of the mods folder (this repository's `mods` unless changed), with the enabled flag
  written back as the mod manager does it (`tools/leonida_mods.py`); a button opens that manager.
- **Tools**: fetch NVIDIA's DLSS files (`d3d12\Get-NVIDIA-DLSS.cmd`, after a confirmation: about 70 MB from NVIDIA's
  GitHub), rebuild the runtime (`d3d12\scripts\build-nvidia.ps1`), open the logs folder
  (`%LOCALAPPDATA%\Leonida\logs`) and the screenshots folder. The two scripts are Windows only.
- **About**: what the port is, that it runs your own copy, the credits (see `d3d12/README.md`) and the licence.

## Settings file

`%APPDATA%\Leonida\launcher.json` (Linux: `~/.config/leonida/launcher.json`), written when you press Save or close the
window. Unknown or mistyped keys are ignored.

## Tests

`python -I tests/test_leonida_launcher.py` covers the parts without a window: reading and writing the settings, the
command line each option produces (checked against the switch names in `scripts/play-d3d12.ps1`), the mod list and
the newest-picture search.
