#!/bin/sh
# Local one-shot: fetch latest upstream krkrsdl2 into ./krkrsdl2, apply the port, build.
set -e
cd "$(dirname "$0")"
[ -d krkrsdl2 ] || git clone --recursive https://github.com/krkrsdl2/krkrsdl2
cd krkrsdl2
git checkout -- . && git -C external/krkrz checkout -- . 2>/dev/null || true
git pull --ff-only && git submodule update --init --recursive
python3 ../tools/fate-port.py .
# bundled SDL2 is old: its PipeWire backend does not compile against current PipeWire headers
cmake -S . -B build -DOPTION_ENABLE_EXTERNAL_PLUGINS=OFF -DSDL_PIPEWIRE=OFF $CMAKE_EXTRA
cmake --build build -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
echo "built: $(pwd)/build/fate-sdl"
# sanity check that the font fixes are really in the tree that was just built
grep -q "take the first candidate without checking" external/krkrz/visual/FreeTypeFontRasterizer.cpp && echo "font fix: present" || echo "font fix: MISSING"
