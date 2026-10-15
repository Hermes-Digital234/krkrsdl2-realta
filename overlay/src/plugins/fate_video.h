/* SPDX-License-Identifier: MIT */
/*
 * Movie playback for the Fate/stay night Realta Nua port.
 *
 * The engine's own VideoOverlay backends are Windows-only (DirectShow /
 * Media Foundation). This is a small replacement: it runs the system's
 * ffmpeg as a decoder (raw BGRA frames and raw PCM over pipes, nothing is
 * linked), plays the audio through SDL and keeps the video on the audio clock.
 * ffmpeg and ffprobe must be installed and on PATH.
 *
 * Everything here is called from the main thread.
 */
#ifndef FATE_VIDEO_H
#define FATE_VIDEO_H

#include <stdint.h>
#include <string>

struct SDL_Renderer;
struct FateMovie;

// Probes the file and prepares playback. On failure returns NULL and fills err.
FateMovie *FateMovieOpen(const std::string &path, std::string &err);
void FateMovieFree(FateMovie *m);

int FateMovieWidth(const FateMovie *m);
int FateMovieHeight(const FateMovie *m);
double FateMovieFPS(const FateMovie *m);
double FateMovieDuration(const FateMovie *m);   // seconds
int FateMovieFrameCount(const FateMovie *m);
bool FateMovieHasAudio(const FateMovie *m);

void FateMoviePlay(FateMovie *m);                // start or resume (restarts after the end)
void FateMoviePause(FateMovie *m);
void FateMovieStop(FateMovie *m);                // stop and go back to the start
void FateMovieSeek(FateMovie *m, double seconds);
bool FateMovieIsPlaying(const FateMovie *m);
double FateMovieTime(const FateMovie *m);        // current position in seconds
void FateMovieSetVolume(FateMovie *m, double v); // 0..1

// Call regularly while playing. Returns true if a new frame became current.
// *ended is set when the stream has been played to its end.
bool FateMovieTick(FateMovie *m, bool *ended);
const uint8_t *FateMovieFrame(const FateMovie *m);  // BGRA, width*height*4, or NULL before the first frame
int FateMovieFrameIndex(const FateMovie *m);

// Overlay: the frame is drawn over the game window by the renderer.
void FateMovieSetOverlay(FateMovie *m, int x, int y, int w, int h, bool visible);
void FateMovieClearOverlay(FateMovie *m);        // only if m is the current overlay
bool FateMovieOverlayDirty();                    // a new frame needs presenting
void FateMovieDrawOverlay(SDL_Renderer *r, int windowWidthHint);

#endif
