#!/bin/sh
# makes a 4 second test clip (colour pattern + 440 Hz tone) next to this script
cd "$(dirname "$0")" || exit 1
ffmpeg -v error -y -f lavfi -i testsrc=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100 \
  -t 4 -c:v libx264 -pix_fmt yuv420p -c:a aac t.mp4 || \
ffmpeg -v error -y -f lavfi -i testsrc=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100 \
  -t 4 -c:v mpeg4 -c:a mp2 t.mp4
echo "now run:  ./fate-sdl tests/movie/   (you should see the pattern and hear a tone; fate-debug.log shows ready, play, stop)"
