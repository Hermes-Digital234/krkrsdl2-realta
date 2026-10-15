# Plugin interface specification

This document describes the *interfaces* that Fate/stay night Realta Nua's scripts expect
from the Windows plugins it links with `Plugins.link()`, and what this project provides
instead. It is derived from **how the game's own scripts call the plugins** (argument lists,
return values, what the caller does with the result), from the public Kirikiri2 / Kirikiri Z
documentation and source, and from public file-format specifications. It contains no code
from any plugin binary.

`Plugins.link()` is a no-op in this build (`OPTION_ENABLE_EXTERNAL_PLUGINS=OFF`);
`src/plugins/fate_compat.cpp` installs the replacement when the script asks for the plugin.

| Plugin (Windows) | What the game uses | Provided here | Status |
|---|---|---|---|
| `KAGParser.dll` | `KAGParser` class | upstream Kirikiri2 source, vendored in `src/plugins/kagparser/` | full |
| `menu.dll` | `MenuItem`, `Window.menu` | `MenuItem` class + on-screen menu bar (SDL has no native menu bar) | full (own design) |
| `fstat.dll` | `Storages.fstat/deleteFile/moveFile/copyFile/createDirectory/removeDirectory/isExistentDirectory` | native, POSIX | full |
| `dirlist.dll` | `getDirList(dir)` | native, POSIX | full |
| `scriptsEx.dll` | `Scripts.getObjectKeys(obj)` | native | full |
| `addFont.dll` | `System.addFont(storage)` | maps to core `Font.addFont` | full |
| `wuvorbis.dll` | `.ogg` decoding | `stb_vorbis` (public domain) | full |
| `wutcwf.dll` | `.tcw` decoding | port of the open-source Kirikiri2 decoder (see PROVENANCE) | full |
| `wumsadp.dll` | MS-ADPCM `.wav` | public ADPCM spec | full |
| `kri18n.dll` | `__()`, `_x()`, `loadMoFile()` | game's `i18n.tjs` + native GNU gettext `.mo` reader | full |
| `util.dll` | see below | TJS shim | partial (see below) |
| `drawer.dll` | `drawLine`, `drawAALine`, `drawAATriangle` | native Bresenham / Wu lines (`fate_effects.cpp`) | lines: full, triangle: inert |
| `filter.dll` | `Noise`, `Blur`, `Stretch`, `Smudge`, `Lens`, `Contrast`, `Vortex`, haze | `Noise`, `Blur`, `Stretch` implemented; rest inert | partial |
| `resize.dll` | `AreaAverageReducation(src, dst)` | linear stretch approximation | approximate |
| `firespark.dll`, `slideopen.dll` | `init/draw/change/finish*` effect calls | inert | not implemented |
| `wallpaper.dll` | `setwallpaper`, `getwallpaper` | inert (sets the Windows desktop background) | n/a |
| `windowEx.dll`, `win32dialog.dll`, `layerExSave.dll`, `layerExAreaAverage.dll`, `shrinkCopy.dll`, `imagesaver.dll` | window/layer extras | core engine equivalents where they exist | see issues |

## Call-site contracts (observed from the scripts)

* `util.dll`: `DeleteFile(path)`, `MoveFile(a,b)`, `CopyFile(a,b)`, `CreateDirectory(p)`,
  `ExistDirectory(p) -> bool`, `GetFileSize(p) -> int (-1 if absent)`,
  `GetDatetime() -> %[year,mon,day,hour,minute,second]` (`year` is `Date.getYear()`),
  `SelectDirectory(caption, initial) -> string` (the caller treats an empty string as
  "cancelled" and falls back to the install directory), `enabledIME(bool)`,
  `SetClipboardText(s)`, `IsActiveWindow(win) -> bool`, `enumFont(win, fonts)`.
* `drawLine(layer, x1, y1, x2, y2, color)` / `drawAALine(...)`: draw a 1 px line on the
  layer's main image; `color` is `0xRRGGBB` or `0xAARRGGBB`.
* `Noise(%[layer: Layer, monocro: bool])`: replace the layer's image with random noise,
  grey when `monocro`, otherwise coloured. Called before the layer's `type`/`opacity` are set.
* `Blur(%[layer, level])`: box-blur the layer by `level`.
* `Stretch(%[src, dest, dleft, dtop, dwidth, dheight, sleft, stop, swidth, sheight, opacity])`:
  blit `src` rect scaled into `dest` rect.
* `AreaAverageReducation(src, dst)`: shrink `src` to the size of `dst`.
* Effects `Smudge`, `Lens/InitLens/ReleaseLens`, `Contrast`, `Vortex`, `initHaze/doHaze/endHaze`,
  `*FireSpark`, `*SlideOpen`: the scripts pass parameter dictionaries, but the *visual result*
  cannot be inferred from call sites, so they are inert. The game continues without the effect.

## File formats

* Ogg Vorbis: Xiph specification (decoded with stb_vorbis).
* MS-ADPCM / IMA: Microsoft RIFF WAVE formats, public.
* TCWF (`.tcw`): Kirikiri2's own sound format; the reference decoder is part of the open-source
  Kirikiri2 tree (`src/plugins/win32/wutcwf`).
* GNU gettext `.mo`: documented by the GNU gettext manual (magic `0x950412de`, string tables,
  optional context via `\x04`).
