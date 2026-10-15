# Provenance

Where each part of this project comes from. See `PLUGIN-INTERFACES.md` for the interface spec.

| Component | Source | License |
|---|---|---|
| krkrsdl2 engine (base) | Kirikiri SDL2 Developers | MIT-style, `LICENSE` |
| `src/plugins/kagparser/` | Kirikiri2 (W.Dee) `KAGParser` plugin, with minimal integration edits | Kirikiri2 license / GPLv2 (dual) |
| `fate_audio.cpp` TCWF + MS-ADPCM decoders | structure follows the open-source Kirikiri2 `wutcwf` / `wumsadp` plugins (W.Dee) | Kirikiri2 license / GPLv2 (dual) |
| `stb_vorbis.c` | Sean Barrett, v1.22 | public domain / MIT |
| `fate_compat.cpp` (file ops, `.mo` reader, `MenuItem`, menu bar overlay, shims) | original | MIT |
| `fate_effects.cpp` | original (Bresenham, Xiaolin Wu, xorshift) | MIT |
| engine patches (fonts, fullscreen clear, plugin hook) | original | MIT |

## About the proprietary game plugins

Several of the game's plugins (`util`, `drawer`, `filter`, `resize`, `firespark`, `slideopen`,
`wallpaper`) are not part of any open-source release. Early in development, decompiler output
of those DLLs was used to understand what the game needs from them (interoperability analysis).
That output is **not** part of this repository and is not redistributed. The code here was then
written against the call-site contracts in `PLUGIN-INTERFACES.md`, using textbook algorithms and
public APIs only; effects whose result could not be specified from call sites were left inert
rather than reconstructed from the binaries.

This is not a formal clean room. This project was made possible by putting the decompiler output of the games dll's into an llm to create the requirements that I (Hermes)
then used to build the plugins. I have never looked into the decompiler output myself, but I did ask the same llm for advice.

## Not included

No game data, scripts, fonts, translations or binaries from Fate/stay night Realta Nua are
included. You need your own legitimate copy of the game.


## Movie playback
`src/plugins/fate_video.cpp` and `fate_video_glue.cpp` are original code written for this port from the public
VideoOverlay interface of the engine (properties, events and status names used by the game scripts). They do not contain or
link any codec: ffmpeg/ffprobe are started as separate processes found on `PATH` and talked to through pipes.
