# Ultrawide (21:9, 32:9)

How the output size goes from the window to the title and back to the screen, and what the opt-in ultrawide settings
change. **Measured** means a run, a test or the code shows it (file:line quoted, line numbers as of this note);
**hypothesis** means nobody has checked it yet.

## Settings

| variable | default | what it changes |
|---|---|---|
| `LEONIDA_ASPECT` | unset | `21:9`, `32:9`, `43:18`, any `W:H` or decimal from 1 to 4, or `auto` (the primary monitor's shape). The display mode the console output reports is widened to that aspect at the same height, and so is the title's window. `off`, `none` or empty: unset. |
| `LEONIDA_PRESENT_SCALE` | `stretch`, or `fit` with `LEONIDA_ASPECT` | `fit`: a frame whose shape is not the window's keeps its aspect, centred, with black bars beside it (pillarbox) or above and below (letterbox). A frame of the window's shape fills it. `stretch`: the frame fills the window, as before. |

`scripts/play.ps1 -Aspect 21:9` (and `-PresentScale fit|stretch`) sets them. The arithmetic is in
`runtime/src/aspect_fit.h` and `runtime/test/aspect_fit_test.cpp` tests it.

| `LEONIDA_DISPLAY_MODE` | `LEONIDA_ASPECT` | the title is told |
|---|---|---|
| unset (3840x2160) | `21:9` | 5040x2160 |
| unset | `32:9` | 7680x2160 |
| `2560x1440` | `21:9` | 3360x1440 |
| `2560x1440` | `43:18` or `auto` on a 3440x1440 monitor | 3440x1440 |
| `2560x1440` | `32:9` | 5120x1440 |

## From window to title to present

1. **The display mode the title is told.** `rt::d3d12::title_display_mode()` (`runtime/shims/xbox_display.cpp:324`)
   answers the console output's slot 21 (`output_modes`, `:230`), its `GetDesc` desktop coordinates (`:178`),
   `GetDisplayModeList` (`:202`) and `FindClosestMatchingMode`. It reads `LEONIDA_DISPLAY_MODE` (default 3840x2160,
   `runtime/src/display_mode.h`) and now widens the result with `LEONIDA_ASPECT` (`title_aspect_ratio()`, `:294`).
   Measured (docs/port_status.md, "The title makes its window at 3840x2160"): the 1.0.2.1 title calls slot 21 just
   before its first `SetFrameIntervalX`, but its window and back buffers stay 3840x2160 whatever slot 21 reports.
   1920x1080 was tried at 60 and 120 Hz.
2. **The size the title renders at.** This size is compiled into the executable: the Series X SDR mode at RVA
   `0x0615c100` (`runtime/src/display_profile.h:46`), the scanout reservation, the temporal scene scale and the
   Settings viewport. `runtime/src/main.cpp:65-82` patches these when `LEONIDA_DISPLAY_MODE=2560x1440`. Every other
   output is refused (`display_profile.h:100`). Measured: with the SDR mode set to 1920x1080 the title still made
   2560x1440 buffers. `LEONIDA_ASPECT` does not change the executable.
3. **The window.** The title makes its own top-level window. `runtime/shims/title_window.cpp:36` (`fit`) shrinks it
   to the work area, keeping its shape. With `LEONIDA_ASPECT` it now widens the window first, keeping the height. When
   the title has no window, the runtime makes one at the first plane's size (`runtime/shims/xbox_window.cpp:496-509`),
   and that one is widened the same way.
4. **The swapchain** follows the window's client area: `currentExtent`, or the `GetClientRect` callback when the
   surface leaves it open (`runtime/src/vulkan_present.h:1401-1405`; the callback is in
   `runtime/shims/d3d12_vulkan_device.cpp`, `create_window_presenter`).
5. **The present.** `XboxPresentation::PresentX` (`runtime/shims/xbox_present.cpp`) sends a plain SDR plane to
   `VulkanPresenter::present` (`:326`). It sends a plane with a placement, a filter, HDR10 or an overlay to
   `present_planes` (`:236`).
   - Before this change both paths stretched. The plain blit's destination was the whole swapchain image (`dstOffsets`
     0..extent). The composed planes' placements were fractions times the window size. A 16:9 frame in a 21:9 window
     was stretched by 1.31.
   - Now, with `Scaling::fit`, the plain blit clears the swapchain image to black and blits into `aspect::fit_box`
     (`vulkan_present.h:879-898`). The composed path puts every placement into the same box (`:991-1009`), and the
     pass already writes black outside plane 0 (`runtime/src/present_compose.comp:140`). The box comes from plane 0's
     source rectangle and placement (`aspect::content_aspect`): a 16:9 frame is pillarboxed and a frame of the window's
     shape fills it. Shapes that differ by less than a pixel fill the window.
   - The default stays `stretch` when `LEONIDA_ASPECT` is unset, so the existing present tests are unchanged. One of
     them, `xbox_present_test`, checks that a half-width source rectangle is stretched over the window.

## What is known and what is not

- Measured (unit test only): the parsers, the widened modes in the table above, and the pillarbox and letterbox
  boxes. `aspect_fit_test` passes on Windows with MSVC.
- Not yet run on a GPU: the fit blit and the fit compose. Neither has been checked on swapchain pixels yet. A
  `xbox_present_test` case with `set_scaling(fit)` in a 21:9 window would cover both.
- Hypothesis: the title keeps rendering 16:9 under `LEONIDA_ASPECT`, because its frame size is the compiled SDR mode
  (item 2). In that case the visible result is a wide window with a pillarboxed 16:9 frame: correct, but not yet
  "ultrawide".
- Unmeasured, needs the game running:
  - whether the title reads slot 21's width anywhere: camera aspect or FOV, HUD anchoring, the Settings screen's
    resolution list;
  - whether extending `display_profile.h` to a wide SDR mode (for example 3440x1440, with the scanout reservation
    from `scanout_pool_bytes`) makes the title render wide. 1920x1080 did not take (it gave 2560x1440), which
    suggests that the title clamps or picks from a list. A wide mode may be refused the same way;
  - if it does render wide: whether the projection follows the back buffer's aspect (Hor+) or is fixed at 16:9
    (vertical stretch or crop), and whether the HUD and menus stay in the centre 16:9. Any of these may need a patch
    of its own later.
- Not covered: exclusive full-screen and monitor mode changes. The window stays a desktop window.
