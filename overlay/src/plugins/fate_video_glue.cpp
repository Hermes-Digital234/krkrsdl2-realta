/* SPDX-License-Identifier: MIT */
/*
 * Connects fate_video.cpp (ffmpeg based decoder) to the engine's VideoOverlay
 * class. The Windows build of the engine uses DirectShow / Media Foundation
 * here; on every other platform VideoOvlImpl.cpp forwards to these members
 * (see FATE_VIDEO in VideoOvlImpl.h).
 */

#include "tjsCommHead.h"

#include <string.h>
#include <stdio.h>
#include <string>

#include "VideoOvlImpl.h"

#ifdef FATE_VIDEO

#include "MsgIntf.h"
#include "DebugIntf.h"
#include "LayerIntf.h"
#include "EventIntf.h"
#include "StorageIntf.h"
#include "UtilStreams.h"
#include "fate_video.h"

struct FateVOState : public tTVPContinuousEventCallbackIntf
{
	tTJSNI_VideoOverlay *owner;
	FateMovie *movie = nullptr;
	bool hooked = false;
	bool nullMovie = false;     // the movie could not be opened: it "plays" for zero time so the game carries on
	tjs_int volume = 100000;
	tjs_int balance = 0;
	tjs_real rate = 1.0;

	explicit FateVOState(tTJSNI_VideoOverlay *o) : owner(o) {}

	void TJS_INTF_METHOD OnContinuousCallback(tjs_uint64) override { owner->FateTick(); }

	void Hook(bool on)
	{
		if (on == hooked) return;
		hooked = on;
		if (on) TVPAddContinuousEventHook(this);
		else TVPRemoveContinuousEventHook(this);
	}
};

extern void TVPFateDebugLog(const char *msg);

static void Log(const ttstr &s)
{
	TVPAddLog(TJS_W("(fate) movie: ") + s);
	TVPFateDebugLog((std::string("movie: ") + s.AsNarrowStdString()).c_str());
}

bool tTJSNI_VideoOverlay::FateOpen(const ttstr &_name)
{
	Close();

	if (!Window) TVPThrowExceptionMessage(TVPWindowAlreadyMissing);

	ttstr name(_name);
	const tjs_char *q = TJS_strchr(name.c_str(), TJS_W('?'));
	if (q) name = ttstr(name, (int)(q - name.c_str()));

	FateVOState *st = new FateVOState(this);

	// make sure the storage exists (throws like the other backends), and get a real file for ffmpeg
	name = TVPSearchPlacedPath(name); // finds it through the auto path, throws if it does not exist
	LocalTempStorageHolder = new tTVPLocalTempStorageHolder(TVPNormalizeStorageName(name));
	std::string path = LocalTempStorageHolder->GetLocalName().AsNarrowStdString();

	std::string err;
	st->movie = FateMovieOpen(path, err);
	if (!st->movie)
	{
		st->nullMovie = true;
		Log(name + TJS_W(": ") + ttstr(err.c_str()) + TJS_W(" - skipping the movie"));
	}
	else
	{
		char b[160];
		snprintf(b, sizeof b, " %dx%d %.3f fps, %.1f s, audio %s", FateMovieWidth(st->movie), FateMovieHeight(st->movie),
			FateMovieFPS(st->movie), FateMovieDuration(st->movie), FateMovieHasAudio(st->movie) ? "yes" : "no");
		Log(name + ttstr(b));
	}
	FateState = st;
	st->Hook(true);

	ClearWndProcMessages();
	if (Mode == vomMFEVR) SetStatusAsync(tTVPVideoOverlayStatus::Ready);
	else SetStatus(tTVPVideoOverlayStatus::Stop);
	FateApplyOverlay();
	return true;
}

void tTJSNI_VideoOverlay::FateClose(bool fire)
{
	FateVOState *st = FateState;
	if (st)
	{
		FateState = nullptr;
		st->Hook(false);
		if (st->movie) FateMovieFree(st->movie);
		delete st;
	}
	if (LocalTempStorageHolder) delete LocalTempStorageHolder, LocalTempStorageHolder = NULL;
	ClearWndProcMessages();
	if (fire) SetStatus(tTVPVideoOverlayStatus::Unload);
	else
	{
		bool c = CanDeliverEvents;
		CanDeliverEvents = false;
		SetStatus(tTVPVideoOverlayStatus::Unload);
		CanDeliverEvents = c;
	}
}

void tTJSNI_VideoOverlay::FatePlay()
{
	FateVOState *st = FateState;
	if (!st) return;
	if (st->movie) FateMoviePlay(st->movie);
	ClearWndProcMessages();
	if (Mode != vomMFEVR) SetStatus(tTVPVideoOverlayStatus::Play);
	else SetStatusAsync(tTVPVideoOverlayStatus::Play);
}

void tTJSNI_VideoOverlay::FateStop()
{
	FateVOState *st = FateState;
	if (!st) return;
	if (st->movie) FateMovieStop(st->movie);
	ClearWndProcMessages();
	SetStatus(tTVPVideoOverlayStatus::Stop);
}

void tTJSNI_VideoOverlay::FatePause()
{
	FateVOState *st = FateState;
	if (!st) return;
	if (st->movie) FateMoviePause(st->movie);
	if (Mode != vomMFEVR) SetStatus(tTVPVideoOverlayStatus::Pause);
	else SetStatusAsync(tTVPVideoOverlayStatus::Pause);
}

void tTJSNI_VideoOverlay::FateRewind()
{
	FateVOState *st = FateState;
	if (!st) return;
	if (st->movie) FateMovieSeek(st->movie, 0);
	ClearWndProcMessages();
	if (EventFrame >= 0 && IsEventPast) IsEventPast = false;
}

void tTJSNI_VideoOverlay::FatePrepare()
{
	if (FateState && Mode == vomLayer)
	{
		Pause();
		Rewind();
		IsPrepare = true;
		Play();
	}
}

void tTJSNI_VideoOverlay::FateApplyOverlay()
{
	FateVOState *st = FateState;
	if (!st || !st->movie) return;
	if (Mode == vomLayer)
	{
		FateMovieClearOverlay(st->movie);
		if (Layer1) Layer1->SetVisible(Visible);
		if (Layer2) Layer2->SetVisible(Visible);
	}
	else
	{
		FateMovieSetOverlay(st->movie, Rect.left, Rect.top, Rect.get_width(), Rect.get_height(), Visible);
	}
}

static void CopyToLayer(tTJSNI_BaseLayer *l, const uint8_t *px, int w, int h)
{
	if (!l) return;
	if ((int)l->GetImageWidth() != w || (int)l->GetImageHeight() != h) l->SetImageSize(w, h);
	if ((int)l->GetWidth() != w || (int)l->GetHeight() != h) l->SetSize(w, h);
	uint8_t *dst = (uint8_t *)l->GetMainImagePixelBufferForWrite();
	tjs_int pitch = l->GetMainImagePixelBufferPitch();
	if (!dst) return;
	for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * pitch, px + (size_t)y * w * 4, (size_t)w * 4);
	l->Update();
}

void tTJSNI_VideoOverlay::FateTick()
{
	FateVOState *st = FateState;
	if (!st) return;

	if (st->nullMovie)
	{
		if (Status == tTVPVideoOverlayStatus::Play) SetStatusAsync(tTVPVideoOverlayStatus::Stop);
		return;
	}

	bool ended = false;
	bool changed = FateMovieTick(st->movie, &ended);

	if (changed && Status == tTVPVideoOverlayStatus::Play)
	{
		int frame = FateMovieFrameIndex(st->movie);
		if (!IsPrepare && SegLoopEndFrame > 0 && frame >= SegLoopEndFrame)
		{
			SetFrame(SegLoopStartFrame > 0 ? SegLoopStartFrame : 0);
			FirePeriodEvent(perSegLoop);
			return;
		}
		if (Mode == vomLayer)
		{
			const uint8_t *px = FateMovieFrame(st->movie);
			if (px)
			{
				CopyToLayer(Layer1, px, FateMovieWidth(st->movie), FateMovieHeight(st->movie));
				CopyToLayer(Layer2, px, FateMovieWidth(st->movie), FateMovieHeight(st->movie));
			}
		}
		FireFrameUpdateEvent(frame);
		if (FateState != st) return; // the handler closed the movie

		if (!IsPrepare)
		{
			if (EventFrame >= 0 && !IsEventPast && frame >= EventFrame)
			{
				EventFrame = -1;
				FirePeriodEvent(perPeriod);
				if (FateState != st) return;
			}
		}
		else
		{
			FirePeriodEvent(perPrepare);
			if (FateState != st) return;
			Pause();
			Rewind();
			IsPrepare = false;
		}
	}

	if (ended && Status == tTVPVideoOverlayStatus::Play)
	{
		if (Loop)
		{
			FateMovieSeek(st->movie, 0);
			FateMoviePlay(st->movie);
			FirePeriodEvent(perLoop);
		}
		else
		{
			SetStatusAsync(tTVPVideoOverlayStatus::Stop);
		}
	}
}

// what: 0 width, 1 height, 2 fps, 3 frame count, 4 total time (ms), 5 position (ms), 6 frame,
//       7 audio streams, 8 enabled audio stream, 9 volume, 10 balance, 11 play rate, 12 video streams
bool tTJSNI_VideoOverlay::FateQuery(int what, double &out)
{
	FateVOState *st = FateState;
	if (!st) return false;
	FateMovie *m = st->movie;
	switch (what)
	{
	case 0: out = m ? FateMovieWidth(m) : 800; return true;
	case 1: out = m ? FateMovieHeight(m) : 600; return true;
	case 2: out = m ? FateMovieFPS(m) : 25; return true;
	case 3: out = m ? FateMovieFrameCount(m) : 0; return true;
	case 4: out = m ? FateMovieDuration(m) * 1000.0 : 0; return true;
	case 5: out = m ? FateMovieTime(m) * 1000.0 : 0; return true;
	case 6: out = m ? FateMovieTime(m) * FateMovieFPS(m) : 0; return true;
	case 7: out = (m && FateMovieHasAudio(m)) ? 1 : 0; return true;
	case 8: out = (m && FateMovieHasAudio(m)) ? 0 : -1; return true;
	case 9: out = st->volume; return true;
	case 10: out = st->balance; return true;
	case 11: out = st->rate; return true;
	case 12: out = m ? 1 : 0; return true;
	}
	return false;
}

bool tTJSNI_VideoOverlay::FateSet(int what, double v)
{
	FateVOState *st = FateState;
	if (!st) return false;
	FateMovie *m = st->movie;
	switch (what)
	{
	case 5: if (m) FateMovieSeek(m, v / 1000.0); return true;
	case 6:
		if (m) FateMovieSeek(m, v / FateMovieFPS(m));
		if (EventFrame >= v && IsEventPast) IsEventPast = false;
		return true;
	case 9:
		st->volume = (tjs_int)v;
		if (m) FateMovieSetVolume(m, v / 100000.0);
		return true;
	case 10: st->balance = (tjs_int)v; return true;
	case 11: st->rate = v; return true;
	}
	return false;
}

#endif // FATE_VIDEO
