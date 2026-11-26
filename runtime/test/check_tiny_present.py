"""Run tiny_present under `gta6rt --run` on the Vulkan D3D12 device and check, from outside, what it presented.

    python -I check_tiny_present.py <gta6rt.exe> <tiny_present.exe> [--probe d3d12_com_test.exe] [--title-window]

The stand-in title (test/fixtures/tiny_present.cpp) asks the console output for the display mode, makes a back buffer
of that size, and presents three-coloured frames through the console queue's PresentX, paced by the device's frame
clock. This sets LEONIDA_DISPLAY_MODE (so the mode is a known one), LEONIDA_CAPTURE_DIR to a new folder and writes 1
into its request.txt before the run, so the runtime saves one presented frame as capture-1.png from its own thread.
Then it checks the guest's verdict, that the display mode the guest got is the one set, that every PresentX of the
d3d12 trace was shown, that the runtime says the frames went to the window (the title's own with --title-window, where
the guest makes one through the API set a title imports CreateWindowExA from, and the shim fits it to the screen), and
the screenshot's pixels: a PNG of the window's client size whose top-left quarter, bottom-left quarter and right half
have the guest's three colours. Exits 77 (ctest: skipped) without a usable Vulkan device or without a desktop to show
a window on. A window shows on the desktop for a few seconds.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

SKIP = 77
DISPLAY_MODE = (1280, 720)
COLOURS = {"top-left": (255, 0, 0), "bottom-left": (64, 128, 191), "right": (0, 200, 0)}


def decode_png(data):
    """(width, height, rows of RGB bytes) of an 8-bit RGB PNG; raises ValueError otherwise."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    at, idat, header = 8, b"", None
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        (crc,) = struct.unpack(">I", data[at + 8 + length:at + 12 + length])
        if zlib.crc32(kind + body) & 0xFFFFFFFF != crc:
            raise ValueError(f"bad CRC in {kind!r}")
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        at += 12 + length
    if not header or header[2:] != (8, 2, 0, 0, 0):
        raise ValueError(f"not 8-bit RGB: {header}")
    width, height = header[0], header[1]
    raw = zlib.decompress(idat)
    stride = width * 3
    if len(raw) != (stride + 1) * height:
        raise ValueError("wrong amount of image data")
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b = previous[i]
            c = previous[i - 3] if i >= 3 else 0
            if kind == 1:
                line[i] = (line[i] + a) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + b) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
            elif kind != 0:
                raise ValueError(f"unknown filter {kind}")
        rows.append(bytes(line))
        previous = line
    return width, height, rows


def check_screenshot(path, window_size):
    """Problems with the saved frame: its size and the three regions' colours (at the middle of each region)."""
    try:
        with open(path, "rb") as f:
            width, height, rows = decode_png(f.read())
    except (OSError, ValueError, zlib.error) as error:
        return [f"the screenshot {path} does not decode: {error}"]
    problems = []
    if window_size and (width, height) != window_size:
        problems.append(f"the screenshot is {width}x{height}, the window's client area {window_size[0]}x{window_size[1]}")
    points = {"top-left": (width // 4, height // 4), "bottom-left": (width // 4, 3 * height // 4),
              "right": (3 * width // 4, height // 2)}
    for name, (x, y) in points.items():
        got = tuple(rows[y][x * 3:x * 3 + 3])
        want = COLOURS[name]
        if any(abs(g - w) > 2 for g, w in zip(got, want)):
            problems.append(f"the {name} region at {x},{y} is {got}, not {want}")
    return problems


def check_run(code, output, title_window):
    problems = []
    lines = output.replace("\r\n", "\n").split("\n")
    if code != 0:
        problems.append(f"exit code {code}")
    if "tiny_present: OK" not in lines:
        problems.append("the guest did not print `tiny_present: OK`")
    if any(line.startswith("tiny_present: FAIL") for line in lines):
        problems.append("the guest reported a failed check")
    mode = next((m for m in (re.fullmatch(r"tiny_present: display mode (\d+)x(\d+) at 60/1 Hz", line) for line in lines)
                 if m), None)
    if not mode or (int(mode[1]), int(mode[2])) != DISPLAY_MODE:
        problems.append(f"the guest was not told the display mode {DISPLAY_MODE[0]}x{DISPLAY_MODE[1]}")
    presents = [line for line in lines if re.fullmatch(r"\[d3d12-trace\] #\d+ XboxPresentation::PresentX \w+", line)]
    shown = [line for line in presents if line.endswith(" ok")]
    if len(shown) < 90 or len(shown) != len(presents):
        problems.append(f"{len(shown)} of {len(presents)} PresentX calls were shown (90 or more, all of them, expected)")
    if not any(line.startswith("[rt] present: the title's frames go to its window") for line in lines):
        problems.append("the runtime did not say the frames go to the window")
    own = any(line.startswith("[rt] window: the title's own window shows its frames") for line in lines)
    if own != title_window:
        problems.append("the frames went to the " + ("runtime's window, not the title's" if title_window
                                                      else "title's window, not the runtime's"))
    saved = next((m for m in (re.match(r"\[rt\] capture: request 1: frame \d+ \((\d+)x(\d+)\) saved as ", line)
                              for line in lines) if m), None)
    if not saved:
        problems.append("the runtime did not save request 1")
    client = next((m for m in (re.fullmatch(r"tiny_present: window client area (\d+)x(\d+)", line) for line in lines)
                   if m), None)
    return problems, (int(client[1]), int(client[2])) if client else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("loader")
    parser.add_argument("guest")
    parser.add_argument("--probe", help="a Vulkan device test; its exit code 77 skips this test")
    parser.add_argument("--title-window", action="store_true", help="the guest makes its own window first")
    options = parser.parse_args()

    if options.probe:
        probe = subprocess.run([options.probe], capture_output=True, timeout=120)
        if probe.returncode == SKIP:
            print("SKIP: no usable Vulkan device (the probe test skipped)")
            return SKIP

    folder = tempfile.mkdtemp(prefix="leonida_tiny_present_")
    try:
        with open(os.path.join(folder, "request.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("1\n")
        environment = dict(os.environ, LEONIDA_D3D12="vulkan", LEONIDA_CAPTURE_DIR=folder,
                           LEONIDA_DISPLAY_MODE=f"{DISPLAY_MODE[0]}x{DISPLAY_MODE[1]}")
        environment.pop("LEONIDA_CAPTURE_REQUEST", None)
        command = [options.loader, "--run", os.path.basename(options.guest), "--trace-d3d12"]
        if options.title_window:
            command += ["--", "--title-window"]
        run = subprocess.run(command, cwd=os.path.dirname(os.path.abspath(options.guest)), env=environment,
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
        output = run.stdout + run.stderr
        if "tiny_present: SKIP" in output:
            print("SKIP: no interactive window station (no desktop to show a window on)")
            return SKIP
        problems, client = check_run(run.returncode, output, options.title_window)
        screenshot = os.path.join(folder, "capture-1.png")
        if os.path.exists(screenshot):
            problems += check_screenshot(screenshot, client)
        else:
            problems.append("no capture-1.png in the capture folder")
        if problems:
            print("\n".join(line for line in output.split("\n") if not line.startswith("[d3d12-trace]")))
            for problem in problems:
                print(f"FAIL: {problem}")
            return 1
        print(f"tiny_present under gta6rt --run ({'the title' if options.title_window else 'the runtime'}'s window): "
              f"OK, screenshot {client[0] if client else '?'}x{client[1] if client else '?'} checked")
        return 0
    finally:
        shutil.rmtree(folder, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
