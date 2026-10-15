# Fate/stay night Realta Nua on Linux (krkrsdl2)

Patches for [krkrsdl2](https://github.com/krkrsdl2/krkrsdl2) that let you run your own copy of
*Fate/stay night [Realta Nua]* natively on Linux, built with `OPTION_ENABLE_EXTERNAL_PLUGINS=OFF`.
The game's Windows plugin DLLs are replaced by in-engine code written against a public
interface spec. **No game data is included; you need your own copy.**

## Requirements

### To build
- `git`
- `python3`
- `cmake`
- a C and C++ compiler (gcc or clang)
- `make` or `ninja`
- development headers for the video and audio backends SDL2 should use (X11, Wayland, OpenGL/Mesa, ALSA or PulseAudio). SDL2, FAudio and zlib come bundled with krkrsdl2, so you don't install them yourself.
- optional: `pkg-config` and the libwebp development package, needed for WebP images

### To run
- Linux with an X11 or Wayland session (Wayland runs through XWayland) and working OpenGL (Mesa is fine)
- sound through ALSA, PulseAudio or PipeWire (through its PulseAudio layer)
- libwebp, if you want WebP images
- `ffmpeg` and `ffprobe` on your `PATH`, for the movies only (without them movies are skipped)
- your own copy of the game (none of its data is included)

## Build (one command)
    ./update.sh            # clones/updates upstream krkrsdl2, applies the port, builds

## How the port is kept up to date
This repo contains no copy of krkrsdl2. `overlay/` holds the new source files and
`tools/fate-port.py` makes small anchor-based edits (idempotent, whitespace-tolerant) to ~8
upstream files. Fork this repo and the included GitHub Action (`sync-upstream.yml`) rebuilds
the port from the newest upstream every night, runs the self-test, and publishes branch `port`.
If upstream changes code an edit anchors on, the script prints `DRIFT <file>: ...` and the job
fails; fix the matching entry in `EDITS` in `tools/fate-port.py` (they're a few lines each).

## What works
Launch, fonts, Ogg/TCWF/ADPCM audio, translation (`.mo`), save/load file operations,
top menu bar (System/Display/Language/Patch/Help; F10 pins, Esc closes),
flowchart lines, noise effect, WebP images (needs libwebp).

Movies: the engine's own movie backends are Windows-only (DirectShow / Media Foundation), so `VideoOverlay` is implemented in `src/plugins/fate_video*.cpp`. ffmpeg is run as a separate program (nothing of it is linked into the executable): one process decodes the picture to raw BGRA frames, another decodes the sound to PCM that is played through SDL, and the picture follows the audio clock. All modes are supported (overlay/Media-Foundation style movies are drawn over the game window at the position and size the game sets; layer mode copies frames into the layers), plus pause, seek, loop, segment loops and the period/frame events. Each movie is logged (`movie: <file> <size> <fps>, <seconds>, audio yes/no`). Not implemented: play rate, stereo balance, brightness/contrast/hue/saturation. Smoke test: `tests/movie/` (see `make-clip.sh`).

Window size: the port remembers the size it asked the window for until the window manager confirms it. Changing the aspect ratio makes the game set the window's width and then its height; SDL reports the old width in between, so the second call could put the old width back and leave the window 4:3 while the picture was 16:9 (wrong mouse positions, stale areas). Window-size requests that differ from the current size are logged as `set window size WxH (was AxB)` in `fate-debug.log`.

Menu bar: sub-menus (the second level and deeper: Patch > Textual Content, Display > Window Size, ...) did not react to the mouse - the layers that catch the clicks were stacked with values that wrapped around a 32-bit integer once there were more than about a dozen of them, which put them underneath the menu. Fixed; every item of every level now highlights and runs.

Menu bar: Window size, Quality and Aspect Ratio rely on the window zoom the game sets with `setZoom()`; the port now remembers that zoom (before, every resize snapped back to 1:1). Help > Index opens `readme.txt` through `xdg-open`. The Windows-only debug console and controller windows are inert stand-ins. Every menu click is traced to `fate-debug.log` (`menu: <caption>`, plus `menu item failed: ...` with the error), which makes a dead item easy to report.

Rendering: GL-type SDL renderers redraw the whole frame on every present (the upstream partial-update trick relies on a two-buffer swap that Mesa software GL and some Wayland drivers do not provide, which garbled small updates such as the exit popup). `SDL_RENDER_DRIVER=software` keeps partial updates. The chosen renderer is logged to `fate-debug.log` (see build options below).

Diagnostics for resize problems: `fate-debug.log` also records `paint box WxH`, `setZoom n/d logical n`, `setInnerSize WxH`, `fullscreen on/off`, `set window size ...` and `output size changed ...`. If the picture, menu bar or mouse positions are off after switching to Wide or fullscreen, attach that log; the order and values of these lines show which sizes disagree.

Build options (CMake, passed as `-DNAME=ON|OFF`):

| Option | Default | Effect |
|---|---|---|
| `OPTION_FATE_DEBUG_LOG` | ON | writes diagnostics (menu clicks, font fallbacks, chosen renderer) to `./fate-debug.log`; OFF writes nothing |
| `OPTION_FATE_VIRTUAL_CURSOR` | OFF | experimental: draw the mouse pointer to emulate the game moving it (Wayland cannot warp it). Pointer positions can be off, so it is not recommended |

Changing either option rebuilds the whole engine.

Known limitation: Wayland compositors do not let programs move the mouse pointer, so the game's
automatic pointer movement onto buttons is skipped there (it works on X11).

## What doesn't work
Fullscreen: Doesn't clear outside the 4:3 window the game runs in.

Widescreen: Menubar coordinates do not line up and the bug found in fullscreen is present here 

## Not implemented (inert)
Haze, lens, vortex, smudge, contrast, fire-spark, slide-open effects; image saver.
See `docs/PLUGIN-INTERFACES.md`.

## Docs
`docs/PLUGIN-INTERFACES.md` (spec), `docs/PROVENANCE.md` (where everything comes from),
`docs/NOTICE.md` (third-party licenses). Self-test: `tests/startup.tjs`
(run `SDL_VIDEODRIVER=dummy ./fate-sdl tests/`, result written to `tests/result.txt`).

New code is MIT; vendored Kirikiri2 parts are under their own license (see NOTICE).
