/* SPDX-License-Identifier: MIT */
/*
 * Movie playback for the Fate/stay night Realta Nua port. See fate_video.h.
 */

#include "fate_video.h"

#include <SDL.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

const int kAudioRate = 48000;
const int kAudioBytesPerSec = kAudioRate * 2 * 2;
const size_t kMaxQueuedFrames = 6;

// ---------------------------------------------------------------------------
// subprocess helpers

pid_t Spawn(const std::vector<std::string> &args, int &readFd)
{
	readFd = -1;
	int p[2];
	if (pipe2(p, O_CLOEXEC) != 0) return -1;

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

	std::vector<char *> argv;
	for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
	argv.push_back(nullptr);

	pid_t pid = -1;
	int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);
	if (rc != 0)
	{
		close(p[0]);
		return -1;
	}
	readFd = p[0];
	return pid;
}

void Reap(pid_t pid, int fd)
{
	if (fd >= 0) close(fd);
	if (pid > 0)
	{
		kill(pid, SIGKILL);
		int st;
		waitpid(pid, &st, 0);
	}
}

// runs a command to completion and returns its stdout
bool Capture(const std::vector<std::string> &args, std::string &out)
{
	int fd;
	pid_t pid = Spawn(args, fd);
	if (pid < 0) return false;
	char buf[4096];
	ssize_t n;
	while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
	close(fd);
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

bool ReadExact(int fd, uint8_t *dst, size_t len)
{
	size_t got = 0;
	while (got < len)
	{
		ssize_t n = read(fd, dst + got, len - got);
		if (n < 0)
		{
			if (errno == EINTR) continue;
			return false;
		}
		if (n == 0) return false;
		got += (size_t)n;
	}
	return true;
}

double ParseRate(const std::string &s)
{
	size_t slash = s.find('/');
	if (slash == std::string::npos) return atof(s.c_str());
	double a = atof(s.substr(0, slash).c_str());
	double b = atof(s.substr(slash + 1).c_str());
	return b > 0 ? a / b : 0;
}

std::string KeyValue(const std::string &text, const char *key)
{
	std::string k = std::string(key) + "=";
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t eol = text.find('\n', pos);
		if (eol == std::string::npos) eol = text.size();
		if (text.compare(pos, k.size(), k) == 0) return text.substr(pos + k.size(), eol - pos - k.size());
		pos = eol + 1;
	}
	return "";
}

// ---------------------------------------------------------------------------

struct Frame
{
	std::vector<uint8_t> px;
	double pts;
};

// one decoding session (a play or seek starts a new one)
struct Run
{
	pid_t vpid = -1, apid = -1;
	int vfd = -1, afd = -1;
	std::thread vth, ath;
	std::mutex mu;
	std::condition_variable cv;
	std::deque<Frame> q;
	bool veof = false;
	std::atomic<bool> aeof{false};
	std::atomic<bool> quit{false};
	std::atomic<uint64_t> aqueued{0};
	std::atomic<bool> audioStarted{false};
};

} // namespace

struct FateMovie
{
	std::string path;
	int w = 0, h = 0;
	double fps = 25, dur = 0;
	bool hasAudio = false;

	// playback
	bool playing = false;
	bool ended = false;
	double base = 0;            // position at which the current run began
	std::shared_ptr<Run> run;
	SDL_AudioDeviceID dev = 0;
	double volume = 1.0;

	// clocks
	bool clockStarted = false;  // no-audio clock
	Uint64 wallStart = 0;
	double wallBase = 0;        // seconds elapsed before the last resume
	bool drained = false;       // audio ran out before the video did
	Uint64 drainedAt = 0;
	double drainedPos = 0;

	// current picture
	std::vector<uint8_t> cur;
	double curPts = 0;
	uint32_t serial = 0;

	double pausedPos = 0;       // position while not playing
};

namespace {

FateMovie *gOverlayMovie = nullptr;
struct OverlayState
{
	int x = 0, y = 0, w = 0, h = 0;
	bool visible = false;
	SDL_Texture *tex = nullptr;
	SDL_Renderer *rend = nullptr;
	int tw = 0, th = 0;
	uint32_t uploaded = 0xffffffffu;
	uint32_t presented = 0xffffffffu;
} gOv;

void ReleaseOverlayTexture()
{
	if (gOv.tex) SDL_DestroyTexture(gOv.tex);
	gOv.tex = nullptr;
	gOv.rend = nullptr;
	gOv.uploaded = 0xffffffffu;
}

void VideoThread(FateMovie *m, std::shared_ptr<Run> r, int w, int h, double fps, double base)
{
	const size_t len = (size_t)w * h * 4;
	for (long idx = 0;; idx++)
	{
		Frame f;
		f.px.resize(len);
		if (!ReadExact(r->vfd, f.px.data(), len)) break;
		f.pts = base + idx / fps;
		std::unique_lock<std::mutex> lk(r->mu);
		r->cv.wait(lk, [&] { return r->quit.load() || r->q.size() < kMaxQueuedFrames; });
		if (r->quit) return;
		r->q.push_back(std::move(f));
	}
	std::lock_guard<std::mutex> lk(r->mu);
	r->veof = true;
}

void AudioThread(FateMovie *m, std::shared_ptr<Run> r, SDL_AudioDeviceID dev, double volume)
{
	std::vector<uint8_t> buf(16384);
	while (!r->quit)
	{
		if (SDL_GetQueuedAudioSize(dev) > (Uint32)(kAudioBytesPerSec / 2))
		{
			SDL_Delay(5);
			continue;
		}
		ssize_t n = read(r->afd, buf.data(), buf.size());
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		n &= ~(ssize_t)3; // whole stereo frames
		if (n <= 0) continue;
		int16_t *s = (int16_t *)buf.data();
		double vol = m->volume;
		if (vol < 0.999)
			for (ssize_t i = 0; i < n / 2; i++) s[i] = (int16_t)(s[i] * vol);
		SDL_QueueAudio(dev, buf.data(), (Uint32)n);
		r->aqueued += (uint64_t)n;
		if (!r->audioStarted && SDL_GetQueuedAudioSize(dev) >= (Uint32)(kAudioBytesPerSec / 4))
		{
			r->audioStarted = true;
			SDL_PauseAudioDevice(dev, 0);
		}
	}
	if (!r->audioStarted)
	{
		r->audioStarted = true;
		SDL_PauseAudioDevice(dev, 0);
	}
	r->aeof = true;
}

void StopRun(FateMovie *m)
{
	std::shared_ptr<Run> r = m->run;
	if (!r) return;
	r->quit = true;
	r->cv.notify_all();
	// killing the processes closes the pipes, which unblocks the reader threads
	if (r->vpid > 0) kill(r->vpid, SIGKILL);
	if (r->apid > 0) kill(r->apid, SIGKILL);
	if (r->vth.joinable()) r->vth.join();
	if (r->ath.joinable()) r->ath.join();
	Reap(r->vpid, r->vfd);
	Reap(r->apid, r->afd);
	if (m->dev)
	{
		SDL_PauseAudioDevice(m->dev, 1);
		SDL_ClearQueuedAudio(m->dev);
	}
	m->run.reset();
}

bool StartRun(FateMovie *m, double from)
{
	StopRun(m);
	std::shared_ptr<Run> r = std::make_shared<Run>();
	char ss[64];
	snprintf(ss, sizeof ss, "%.3f", from);

	std::vector<std::string> v = {"ffmpeg", "-v", "quiet", "-nostdin", "-ss", ss, "-i", m->path,
		"-map", "0:v:0", "-an", "-sn", "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "bgra", "-"};
	r->vpid = Spawn(v, r->vfd);
	if (r->vpid < 0) return false;

	if (m->hasAudio && m->dev)
	{
		std::vector<std::string> a = {"ffmpeg", "-v", "quiet", "-nostdin", "-ss", ss, "-i", m->path,
			"-map", "0:a:0", "-vn", "-sn", "-f", "s16le", "-ar", std::to_string(kAudioRate), "-ac", "2", "-"};
		r->apid = Spawn(a, r->afd);
	}

	m->base = from;
	m->ended = false;
	m->drained = false;
	m->clockStarted = false;
	m->wallBase = 0;
	m->run = r;
	r->vth = std::thread(VideoThread, m, r, m->w, m->h, m->fps, from);
	if (r->apid > 0)
		r->ath = std::thread(AudioThread, m, r, m->dev, m->volume);
	else
		r->aeof = true;
	return true;
}

double Clock(FateMovie *m)
{
	std::shared_ptr<Run> r = m->run;
	if (!r) return m->pausedPos;
	if (r->apid > 0 && m->dev)
	{
		if (!r->audioStarted) return m->base;
		Uint32 queued = SDL_GetQueuedAudioSize(m->dev);
		uint64_t total = r->aqueued;
		if (r->aeof && queued == 0)
		{
			// the audio track is shorter than the video: carry on with the wall clock
			if (!m->drained)
			{
				m->drained = true;
				m->drainedAt = SDL_GetTicks64();
				m->drainedPos = m->base + (double)total / kAudioBytesPerSec;
			}
			return m->drainedPos + (SDL_GetTicks64() - m->drainedAt) / 1000.0;
		}
		uint64_t played = total > queued ? total - queued : 0;
		return m->base + (double)played / kAudioBytesPerSec;
	}
	if (!m->clockStarted) return m->base;
	return m->base + m->wallBase + (SDL_GetTicks64() - m->wallStart) / 1000.0;
}

} // namespace

// ---------------------------------------------------------------------------

FateMovie *FateMovieOpen(const std::string &path, std::string &err)
{
	std::string out;
	if (!Capture({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
	              "stream=width,height,r_frame_rate:format=duration", "-of", "default=noprint_wrappers=1", path}, out))
	{
		err = "could not run ffprobe on the movie (are ffmpeg and ffprobe installed?)";
		return nullptr;
	}
	FateMovie *m = new FateMovie();
	m->path = path;
	m->w = atoi(KeyValue(out, "width").c_str());
	m->h = atoi(KeyValue(out, "height").c_str());
	double fps = ParseRate(KeyValue(out, "r_frame_rate"));
	if (fps > 1 && fps < 240) m->fps = fps;
	m->dur = atof(KeyValue(out, "duration").c_str());
	if (m->w <= 0 || m->h <= 0)
	{
		err = "no video stream found";
		delete m;
		return nullptr;
	}

	std::string aout;
	if (Capture({"ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=codec_type",
	             "-of", "default=noprint_wrappers=1", path}, aout))
		m->hasAudio = aout.find("codec_type=audio") != std::string::npos;

	if (m->hasAudio)
	{
		if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
		SDL_AudioSpec want, have;
		SDL_zero(want);
		want.freq = kAudioRate;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 2048;
		m->dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
		if (m->dev && (have.freq != kAudioRate || have.format != AUDIO_S16SYS || have.channels != 2))
		{
			// the clock arithmetic relies on this exact format
			SDL_CloseAudioDevice(m->dev);
			m->dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
		}
		if (m->dev) SDL_PauseAudioDevice(m->dev, 1);
	}
	return m;
}

void FateMovieFree(FateMovie *m)
{
	if (!m) return;
	FateMovieClearOverlay(m);
	StopRun(m);
	if (m->dev) SDL_CloseAudioDevice(m->dev);
	delete m;
}

int FateMovieWidth(const FateMovie *m) { return m->w; }
int FateMovieHeight(const FateMovie *m) { return m->h; }
double FateMovieFPS(const FateMovie *m) { return m->fps; }
double FateMovieDuration(const FateMovie *m) { return m->dur; }
int FateMovieFrameCount(const FateMovie *m) { return (int)std::lround(m->dur * m->fps); }
bool FateMovieHasAudio(const FateMovie *m) { return m->hasAudio && m->dev; }
bool FateMovieIsPlaying(const FateMovie *m) { return m->playing; }

void FateMoviePlay(FateMovie *m)
{
	if (m->playing) return;
	if (m->ended) m->pausedPos = 0;
	if (!m->run)
	{
		if (!StartRun(m, m->pausedPos)) return;
	}
	else if (m->run->apid <= 0 || !m->dev)
	{
		m->wallStart = SDL_GetTicks64();
		m->clockStarted = true;
	}
	else if (m->dev && m->run->audioStarted)
		SDL_PauseAudioDevice(m->dev, 0);
	m->playing = true;
}

void FateMoviePause(FateMovie *m)
{
	if (!m->playing) return;
	m->pausedPos = Clock(m);
	if (m->run)
	{
		if (m->run->apid > 0 && m->dev)
			SDL_PauseAudioDevice(m->dev, 1);
		else if (m->clockStarted)
		{
			m->wallBase += (SDL_GetTicks64() - m->wallStart) / 1000.0;
			m->clockStarted = false;
		}
	}
	m->playing = false;
}

void FateMovieStop(FateMovie *m)
{
	StopRun(m);
	m->playing = false;
	m->ended = false;
	m->pausedPos = 0;
}

void FateMovieSeek(FateMovie *m, double seconds)
{
	if (seconds < 0) seconds = 0;
	bool was = m->playing;
	StopRun(m);
	m->playing = false;
	m->pausedPos = seconds;
	m->ended = false;
	if (was) FateMoviePlay(m);
}

double FateMovieTime(const FateMovie *m)
{
	if (!m->playing) return m->pausedPos;
	return Clock(const_cast<FateMovie *>(m));
}

void FateMovieSetVolume(FateMovie *m, double v)
{
	m->volume = v < 0 ? 0 : (v > 1 ? 1 : v);
}

bool FateMovieTick(FateMovie *m, bool *ended)
{
	if (ended) *ended = false;
	if (!m->playing || !m->run) return false;
	std::shared_ptr<Run> r = m->run;
	bool changed = false;

	std::unique_lock<std::mutex> lk(r->mu);
	if (!m->clockStarted && (r->apid <= 0 || !m->dev) && !r->q.empty())
	{
		m->wallStart = SDL_GetTicks64();
		m->clockStarted = true;
	}
	double pos = Clock(m);
	while (!r->q.empty() && r->q.front().pts <= pos + 0.002)
	{
		Frame &f = r->q.front();
		m->cur.swap(f.px);
		m->curPts = f.pts;
		m->serial++;
		changed = true;
		r->q.pop_front();
	}
	bool vdone = r->veof && r->q.empty();
	lk.unlock();
	r->cv.notify_all();

	if (vdone)
	{
		bool audioDone = r->apid <= 0 || !m->dev || (r->aeof && SDL_GetQueuedAudioSize(m->dev) == 0);
		// let the last frame stay on screen for its duration
		if (audioDone && pos >= m->curPts + 1.0 / m->fps - 0.001)
		{
			m->playing = false;
			m->ended = true;
			m->pausedPos = 0;
			StopRun(m);
			if (ended) *ended = true;
		}
	}
	return changed;
}

const uint8_t *FateMovieFrame(const FateMovie *m)
{
	return m->cur.empty() ? nullptr : m->cur.data();
}

int FateMovieFrameIndex(const FateMovie *m)
{
	return (int)std::lround(m->curPts * m->fps);
}

// ---------------------------------------------------------------------------
// overlay

void FateMovieSetOverlay(FateMovie *m, int x, int y, int w, int h, bool visible)
{
	if (gOverlayMovie != m)
	{
		ReleaseOverlayTexture();
		gOverlayMovie = m;
	}
	gOv.x = x; gOv.y = y; gOv.w = w; gOv.h = h;
	gOv.visible = visible;
	gOv.presented = 0xffffffffu;
}

void FateMovieClearOverlay(FateMovie *m)
{
	if (gOverlayMovie == m)
	{
		gOverlayMovie = nullptr;
		ReleaseOverlayTexture();
	}
}

bool FateMovieOverlayDirty()
{
	return gOverlayMovie && gOv.visible && gOverlayMovie->serial != gOv.presented;
}

void FateMovieDrawOverlay(SDL_Renderer *r, int windowWidthHint)
{
	FateMovie *m = gOverlayMovie;
	if (!m || !gOv.visible || m->cur.empty() || !r) return;
	(void)windowWidthHint;

	if (gOv.rend != r || !gOv.tex || gOv.tw != m->w || gOv.th != m->h)
	{
		ReleaseOverlayTexture();
		gOv.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, m->w, m->h);
		if (!gOv.tex) return;
		SDL_SetTextureBlendMode(gOv.tex, SDL_BLENDMODE_NONE);
		gOv.rend = r;
		gOv.tw = m->w;
		gOv.th = m->h;
	}
	if (gOv.uploaded != m->serial)
	{
		SDL_UpdateTexture(gOv.tex, nullptr, m->cur.data(), m->w * 4);
		gOv.uploaded = m->serial;
	}
	gOv.presented = m->serial;

	int lw = 0, lh = 0;
	SDL_RenderGetLogicalSize(r, &lw, &lh);
	if (lw <= 0 || lh <= 0) SDL_GetRendererOutputSize(r, &lw, &lh);
	SDL_Rect clip = {0, 0, lw, lh};
	SDL_Rect dst = {gOv.x, gOv.y, gOv.w > 0 ? gOv.w : m->w, gOv.h > 0 ? gOv.h : m->h};
	SDL_RenderSetClipRect(r, &clip);
	SDL_RenderCopy(r, gOv.tex, nullptr, &dst);
	SDL_RenderSetClipRect(r, nullptr);
}
