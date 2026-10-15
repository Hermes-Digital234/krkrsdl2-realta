//---------------------------------------------------------------------------
// fate_audio.cpp - wave decoders that the original game got from plugins
//
//   wuvorbis.dll -> Ogg Vorbis  (*.ogg)     via bundled stb_vorbis
//   wutcwf.dll   -> TCWF        (*.tcw)     ported from kirikiri2 wutcwf
//   wumsadp.dll  -> MS-ADPCM    (*.wav)     ported from kirikiri2 wumsadp
//
// The TCWF and MS-ADPCM decoders follow the open-source Kirikiri2 plugins
// (Copyright (C) W.Dee and contributors; Kirikiri2 license / GPLv2). See
// docs/PROVENANCE.md and docs/NOTICE.md.
//
// krkrsdl2 has no built-in Ogg Vorbis decoder (it expects a loadable plugin)
// and the Windows plugins cannot be loaded here.
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "WaveIntf.h"

#include <memory>
#include <string.h>
#include <vector>

extern "C" {
#define STB_VORBIS_HEADER_ONLY 1
#define STB_VORBIS_NO_STDIO 1
#include "stb_vorbis.c"
}

typedef std::vector<tjs_uint8> FateBytes;

static bool FateReadAll(const ttstr &name, FateBytes &out)
{
	try
	{
		std::unique_ptr<tTJSBinaryStream> s(TVPCreateStream(name));
		if (!s) return false;
		tjs_uint64 size = s->GetSize();
		if (size == 0 || size > 0x7fffffffULL) return false;
		out.resize((size_t)size);
		tjs_uint got = s->Read(&out[0], (tjs_uint)size);
		if (got != size) return false;
		return true;
	}
	catch (...) { return false; }
}

bool TVPFateReadStorage(const ttstr &name, std::vector<tjs_uint8> &out) { return FateReadAll(name, out); }

static void FateFillFormat(tTVPWaveFormat &f, tjs_uint rate, tjs_uint ch, tjs_uint64 total)
{
	memset(&f, 0, sizeof(f));
	f.SamplesPerSec = rate;
	f.Channels = ch;
	f.BitsPerSample = 16;
	f.BytesPerSample = 2;
	f.TotalSamples = total;
	f.TotalTime = (rate && total) ? (total * 1000ULL / rate) : 0;
	f.SpeakerConfig = 0;
	f.IsFloat = false;
	f.Seekable = true;
}

static inline tjs_int16 FateLE16(const tjs_uint8 *p) { return (tjs_int16)(p[0] | (p[1] << 8)); }
static inline tjs_uint16 FateULE16(const tjs_uint8 *p) { return (tjs_uint16)(p[0] | (p[1] << 8)); }
static inline tjs_uint32 FateULE32(const tjs_uint8 *p) { return (tjs_uint32)p[0] | ((tjs_uint32)p[1] << 8) | ((tjs_uint32)p[2] << 16) | ((tjs_uint32)p[3] << 24); }

//---------------------------------------------------------------------------
// Ogg Vorbis
//---------------------------------------------------------------------------
class FateVorbisDecoder : public tTVPWaveDecoder
{
	FateBytes Data;
	stb_vorbis *V;
	tTVPWaveFormat Format;
public:
	FateVorbisDecoder() : V(NULL) {}
	~FateVorbisDecoder() { if (V) stb_vorbis_close(V); }
	bool Open(const ttstr &name)
	{
		if (!FateReadAll(name, Data)) return false;
		int err = 0;
		V = stb_vorbis_open_memory(&Data[0], (int)Data.size(), &err, NULL);
		if (!V) return false;
		stb_vorbis_info info = stb_vorbis_get_info(V);
		if (info.channels < 1 || info.channels > 8) return false;
		FateFillFormat(Format, info.sample_rate, info.channels, stb_vorbis_stream_length_in_samples(V));
		stb_vorbis_seek_start(V);
		return true;
	}
	void GetFormat(tTVPWaveFormat &f) override { f = Format; }
	bool Render(void *buf, tjs_uint bufsamplelen, tjs_uint &rendered) override
	{
		tjs_int16 *out = (tjs_int16 *)buf;
		tjs_uint done = 0;
		while (done < bufsamplelen)
		{
			int n = stb_vorbis_get_samples_short_interleaved(V, (int)Format.Channels,
				out + done * Format.Channels, (int)((bufsamplelen - done) * Format.Channels));
			if (n <= 0) break;
			done += (tjs_uint)n;
		}
		rendered = done;
		return done >= bufsamplelen;
	}
	bool SetPosition(tjs_uint64 pos) override
	{
		return stb_vorbis_seek(V, (unsigned int)pos) != 0;
	}
};

//---------------------------------------------------------------------------
// TCWF (*.tcw)
//---------------------------------------------------------------------------
static const int TcwAdaptationTable[] = {
	230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
static const int TcwCoeff1[] = { 256, 512, 0, 192, 240, 460, 392 };
static const int TcwCoeff2[] = { 0, -256, 0, 64, 0, -208, -232 };
static const int ImaIndexAdjust[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ImaStepSize[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767 };

class FateTcwfDecoder : public tTVPWaveDecoder
{
	FateBytes Data;
	tjs_uint Channels, SamplesPerBlock, BytesPerBlock;
	size_t DataStart, StreamPos;
	std::vector<tjs_int16> Samples; // SamplesPerBlock * Channels, interleaved
	tjs_int SamplePos; // index into Samples (in granules)
	tjs_int BufferRemain;
	tTVPWaveFormat Format;

	static int Clamp(int v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }

	bool ReadBlock(int numchans, int chan)
	{
		const size_t BH = 32; // sizeof(TTCWBlockHeader)
		if (BytesPerBlock <= BH) return false;
		if (StreamPos + BytesPerBlock > Data.size()) return false;
		const tjs_uint8 *bh = &Data[StreamPos];
		const tjs_uint8 *blk = bh + BH;
		StreamPos += BytesPerBlock;

		tjs_int16 *S = &Samples[0] + chan;
		S[0 * numchans] = FateLE16(bh + 0);
		S[1 * numchans] = FateLE16(bh + 2);
		int idelta = FateLE16(bh + 4);
		int bpred = bh[6];
		int stepindex = bh[7];
		if (bpred >= 7) return false; // out of sync
		if (stepindex > 88) return false;

		for (tjs_uint k = 2, p = 0; k < SamplesPerBlock; k++, p++)
		{
			int code = blk[p] & 0xF;
			int idelta_save = idelta;
			idelta = (TcwAdaptationTable[code] * idelta) >> 8;
			if (idelta < 16) idelta = 16;
			if (code & 8) code -= 16;
			int predict = ((S[(k - 1) * numchans] * TcwCoeff1[bpred]) + (S[(k - 2) * numchans] * TcwCoeff2[bpred])) >> 8;
			S[k * numchans] = (tjs_int16)Clamp(code * idelta_save + predict);
		}

		int prev = 0;
		for (tjs_uint k = 2, p = 0; k < SamplesPerBlock; k++, p++)
		{
			int code = (blk[p] >> 4) & 0xF;
			int step = ImaStepSize[stepindex];
			int diff = step >> 3;
			if (code & 1) diff += step >> 2;
			if (code & 2) diff += step >> 1;
			if (code & 4) diff += step;
			if (code & 8) diff = -diff;
			int current = Clamp(prev + diff);
			stepindex += ImaIndexAdjust[code];
			if (stepindex < 0) stepindex = 0; else if (stepindex > 88) stepindex = 88;
			prev = current;
			S[k * numchans] = (tjs_int16)Clamp(S[k * numchans] + current);
		}

		for (int i = 0; i < 6; i++)
		{
			const tjs_uint8 *pk = bh + 8 + i * 4;
			int revise = FateLE16(pk + 2);
			if (revise)
			{
				tjs_uint pos = FateULE16(pk);
				if (pos < SamplesPerBlock)
					S[pos * numchans] = (tjs_int16)Clamp(S[pos * numchans] - revise);
			}
		}
		return true;
	}
	bool ReadAllChannels()
	{
		for (tjs_uint c = 0; c < Channels; c++)
			if (!ReadBlock((int)Channels, (int)c)) return false;
		return true;
	}
public:
	FateTcwfDecoder() : Channels(0), SamplesPerBlock(0), BytesPerBlock(0), DataStart(0), StreamPos(0), SamplePos(0), BufferRemain(0) {}
	bool Open(const ttstr &name)
	{
		if (!FateReadAll(name, Data)) return false;
		if (Data.size() < 24 || memcmp(&Data[0], "TCWF0\x1a", 6)) return false;
		Channels = Data[6];
		tjs_uint32 freq = FateULE32(&Data[8]);
		BytesPerBlock = FateULE32(&Data[16]);
		SamplesPerBlock = FateULE32(&Data[20]);
		if (Channels < 1 || Channels > 2 || SamplesPerBlock < 3 || SamplesPerBlock > 65536 || BytesPerBlock <= 32) return false;
		if (BytesPerBlock - 32 < SamplesPerBlock - 2) return false;
		DataStart = StreamPos = 24;
		Samples.assign((size_t)SamplesPerBlock * Channels, 0);
		FateFillFormat(Format, freq, Channels, 0); // total length unknown, like the original
		return true;
	}
	void GetFormat(tTVPWaveFormat &f) override { f = Format; }
	bool Render(void *buf, tjs_uint bufsamplelen, tjs_uint &rendered) override
	{
		tjs_int16 *out = (tjs_int16 *)buf;
		tjs_uint n = 0;
		while (n < bufsamplelen)
		{
			if (BufferRemain <= 0)
			{
				if (!ReadAllChannels()) { rendered = n; return false; }
				SamplePos = 0;
				BufferRemain = (tjs_int)SamplesPerBlock;
			}
			for (tjs_uint c = 0; c < Channels; c++) *out++ = Samples[(size_t)SamplePos * Channels + c];
			SamplePos++; BufferRemain--; n++;
		}
		rendered = n;
		return true;
	}
	bool SetPosition(tjs_uint64 pos) override
	{
		size_t blk = (size_t)(pos / SamplesPerBlock);
		tjs_uint remnant = (tjs_uint)(pos - (tjs_uint64)blk * SamplesPerBlock);
		size_t np = DataStart + blk * BytesPerBlock * Channels;
		if (np > Data.size()) return false;
		StreamPos = np;
		BufferRemain = 0;
		if (!ReadAllChannels()) { BufferRemain = 0; return pos == 0 ? true : false; }
		SamplePos = (tjs_int)remnant;
		BufferRemain = (tjs_int)(SamplesPerBlock - remnant);
		return true;
	}
};

//---------------------------------------------------------------------------
// Microsoft ADPCM in RIFF/WAVE (format tag 2)
//---------------------------------------------------------------------------
static const int MsAdaptation[] = {
	230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };

class FateMsAdpcmDecoder : public tTVPWaveDecoder
{
	FateBytes Data;
	tjs_uint Channels, SamplesPerBlock, BlockAlign;
	std::vector<int> Coef1, Coef2;
	size_t DataStart, DataEnd, StreamPos;
	std::vector<int> Samples;
	tjs_int SamplePos, BufferRemain;
	tTVPWaveFormat Format;

	bool ReadBlock()
	{
		if (StreamPos + BlockAlign > DataEnd) return false;
		const tjs_uint8 *b = &Data[StreamPos];
		StreamPos += BlockAlign;
		const int ch = (int)Channels;
		int pred[2] = { 0, 0 }, delta[2] = { 0, 0 };
		if (ch == 1)
		{
			pred[0] = b[0]; delta[0] = FateLE16(b + 1);
			Samples[1] = FateLE16(b + 3); Samples[0] = FateLE16(b + 5);
		}
		else
		{
			pred[0] = b[0]; pred[1] = b[1];
			delta[0] = FateLE16(b + 2); delta[1] = FateLE16(b + 4);
			Samples[2] = FateLE16(b + 6); Samples[3] = FateLE16(b + 8);
			Samples[0] = FateLE16(b + 10); Samples[1] = FateLE16(b + 12);
		}
		for (int c = 0; c < ch; c++) if (pred[c] >= (int)Coef1.size()) return false;
		size_t p = 7 * (size_t)ch;
		size_t i = 2 * (size_t)ch;
		while (i < (size_t)SamplesPerBlock * ch)
		{
			if (p >= BlockAlign) { Samples[i++] = 0; Samples[i++] = 0; continue; }
			tjs_uint8 v = b[p++];
			Samples[i++] = v >> 4;
			if (i < Samples.size()) Samples[i++] = v & 0x0f;
		}
		size_t q = 2 * (size_t)ch;
		while (q < (size_t)SamplesPerBlock * ch)
		{
			for (int c = 0; c < ch; c++)
			{
				int code = Samples[q] & 0xf;
				int idelta = delta[c];
				delta[c] = (MsAdaptation[code] * idelta) >> 8;
				if (delta[c] < 16) delta[c] = 16;
				int pr = (Samples[q - ch] * Coef1[pred[c]] + Samples[q - 2 * ch] * Coef2[pred[c]]) >> 8;
				int cur = ((code >= 8 ? code - 16 : code) * idelta) + pr;
				if (cur > 32767) cur = 32767; else if (cur < -32768) cur = -32768;
				Samples[q++] = cur;
			}
		}
		SamplePos = 0;
		BufferRemain = (tjs_int)SamplesPerBlock;
		return true;
	}
public:
	FateMsAdpcmDecoder() : Channels(0), SamplesPerBlock(0), BlockAlign(0), DataStart(0), DataEnd(0), StreamPos(0), SamplePos(0), BufferRemain(0) {}
	bool Open(const ttstr &name)
	{
		if (!FateReadAll(name, Data)) return false;
		if (Data.size() < 44 || memcmp(&Data[0], "RIFF", 4) || memcmp(&Data[8], "WAVE", 4)) return false;
		size_t pos = 12;
		bool haveFmt = false, haveData = false;
		tjs_uint rate = 0;
		while (pos + 8 <= Data.size())
		{
			tjs_uint32 sz = FateULE32(&Data[pos + 4]);
			const tjs_uint8 *body = &Data[pos + 8];
			if (!memcmp(&Data[pos], "fmt ", 4))
			{
				if (sz < 22 || pos + 8 + sz > Data.size()) return false;
				if (FateULE16(body) != 2) return false; // not MS-ADPCM
				Channels = FateULE16(body + 2);
				rate = FateULE32(body + 4);
				BlockAlign = FateULE16(body + 12);
				SamplesPerBlock = FateULE16(body + 18);
				tjs_uint ncoef = FateULE16(body + 20);
				if (sz < 22 + ncoef * 4) return false;
				for (tjs_uint i = 0; i < ncoef; i++)
				{
					Coef1.push_back(FateLE16(body + 22 + i * 4));
					Coef2.push_back(FateLE16(body + 24 + i * 4));
				}
				haveFmt = true;
			}
			else if (!memcmp(&Data[pos], "data", 4))
			{
				DataStart = pos + 8;
				DataEnd = DataStart + sz;
				if (DataEnd > Data.size()) DataEnd = Data.size();
				haveData = true;
				break;
			}
			pos += 8 + (size_t)sz + (sz & 1);
		}
		if (!haveFmt || !haveData) return false;
		if (Channels < 1 || Channels > 2 || SamplesPerBlock < 3 || BlockAlign < 7 * Channels || Coef1.empty()) return false;
		StreamPos = DataStart;
		Samples.assign((size_t)SamplesPerBlock * Channels, 0);
		tjs_uint64 total = (tjs_uint64)((DataEnd - DataStart) / BlockAlign) * SamplesPerBlock;
		FateFillFormat(Format, rate, Channels, total);
		return true;
	}
	void GetFormat(tTVPWaveFormat &f) override { f = Format; }
	bool Render(void *buf, tjs_uint bufsamplelen, tjs_uint &rendered) override
	{
		tjs_int16 *out = (tjs_int16 *)buf;
		tjs_uint n = 0;
		while (n < bufsamplelen)
		{
			if (BufferRemain <= 0 && !ReadBlock()) { rendered = n; return false; }
			for (tjs_uint c = 0; c < Channels; c++) *out++ = (tjs_int16)Samples[(size_t)SamplePos * Channels + c];
			SamplePos++; BufferRemain--; n++;
		}
		rendered = n;
		return true;
	}
	bool SetPosition(tjs_uint64 pos) override
	{
		size_t blk = (size_t)(pos / SamplesPerBlock);
		tjs_uint remnant = (tjs_uint)(pos - (tjs_uint64)blk * SamplesPerBlock);
		size_t np = DataStart + blk * BlockAlign;
		if (np > DataEnd) return false;
		StreamPos = np;
		BufferRemain = 0;
		if (!ReadBlock()) return false;
		SamplePos = (tjs_int)remnant;
		BufferRemain = (tjs_int)(SamplesPerBlock - remnant);
		return true;
	}
};

//---------------------------------------------------------------------------
// creators
//---------------------------------------------------------------------------
class FateWaveCreator : public tTVPWaveDecoderCreator
{
public:
	tTVPWaveDecoder *Create(const ttstr &storagename, const ttstr &ext) override
	{
		try
		{
			if (ext == TJS_W(".ogg") || ext == TJS_W(".oga"))
			{
				std::unique_ptr<FateVorbisDecoder> d(new FateVorbisDecoder());
				if (d->Open(storagename)) return d.release();
			}
			else if (ext == TJS_W(".tcw"))
			{
				std::unique_ptr<FateTcwfDecoder> d(new FateTcwfDecoder());
				if (d->Open(storagename)) return d.release();
			}
			else if (ext == TJS_W(".wav"))
			{
				std::unique_ptr<FateMsAdpcmDecoder> d(new FateMsAdpcmDecoder());
				if (d->Open(storagename)) return d.release();
			}
		}
		catch (...) {}
		return NULL;
	}
};
static FateWaveCreator TheFateWaveCreator;

void TVPFateAudioRegister()
{
	static bool done = false;
	if (done) return;
	done = true;
	TVPRegisterWaveDecoderCreator(&TheFateWaveCreator);
}
