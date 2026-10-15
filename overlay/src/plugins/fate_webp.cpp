/* SPDX-License-Identifier: MIT */

/**
 * @file
 * WebP image loader for the graphics loader registry.
 *
 * The game's HD patch ships some images (and the widescreen transition rule
 * images) as .webp. On Windows these are decoded by a Susie plug-in that the
 * scripts link; here a ".webp" loader is registered with the engine's graphic
 * loader instead, using the system libwebp (BSD-3-Clause) for decoding.
 *
 * Only compiled when CMake finds libwebp (KRKRSDL2_FATE_WEBP).
 */

#include "tjsCommHead.h"
#include "GraphicsLoaderIntf.h"
#include "StorageIntf.h"
#include "MsgIntf.h"
#include "tjsDictionary.h"

#include <stdint.h>
#include <string.h>
#include <vector>
#include <webp/decode.h>

namespace {

void ReadAll(tTJSBinaryStream *src, std::vector<uint8_t> &buf)
{
	tjs_uint64 size = src->GetSize();
	buf.resize((size_t)size);
	if (size) src->ReadBuffer(&buf[0], (tjs_uint)size);
}

void LoadWebP(void *formatdata, void *callbackdata, tTVPGraphicSizeCallback sizecallback,
			  tTVPGraphicScanLineCallback scanlinecallback, tTVPMetaInfoPushCallback metainfopushcallback,
			  tTJSBinaryStream *src, tjs_int keyidx, tTVPGraphicLoadMode mode)
{
	std::vector<uint8_t> data;
	ReadAll(src, data);
	if (data.empty()) TVPThrowExceptionMessage(TJS_W("WebP: empty file"));

	int w = 0, h = 0;
	if (!WebPGetInfo(&data[0], data.size(), &w, &h) || w <= 0 || h <= 0 || w >= 65536 || h >= 65536)
		TVPThrowExceptionMessage(TJS_W("WebP: not a valid WebP image"));

	const bool rgba = (mode == glmNormalRGBA);
	uint8_t *pix = rgba ? WebPDecodeRGBA(&data[0], data.size(), &w, &h)
						: WebPDecodeBGRA(&data[0], data.size(), &w, &h);
	if (!pix) TVPThrowExceptionMessage(TJS_W("WebP: decoding failed"));

	try
	{
		sizecallback(callbackdata, (tjs_uint)w, (tjs_uint)h);
		const bool gray = (mode == glmGrayscale || mode == glmPalettized);
		for (int y = 0; y < h; y++)
		{
			void *sl = scanlinecallback(callbackdata, y);
			if (!sl) break;
			const uint8_t *s = pix + (size_t)y * w * 4;
			if (gray)
			{
				uint8_t *d = (uint8_t *)sl;
				for (int x = 0; x < w; x++, s += 4) // BGRA: luma from R,G,B
					d[x] = (uint8_t)((s[2] * 77 + s[1] * 151 + s[0] * 28) >> 8);
			}
			else
				memcpy(sl, s, (size_t)w * 4);
			scanlinecallback(callbackdata, -1);
		}
	}
	catch (...)
	{
		WebPFree(pix);
		throw;
	}
	WebPFree(pix);
}

void LoadHeaderWebP(void *formatdata, tTJSBinaryStream *src, iTJSDispatch2 **dic)
{
	std::vector<uint8_t> data;
	ReadAll(src, data);
	int w = 0, h = 0;
	if (data.empty() || !WebPGetInfo(&data[0], data.size(), &w, &h))
		TVPThrowExceptionMessage(TJS_W("WebP: not a valid WebP image"));
	*dic = TJSCreateDictionaryObject();
	tTJSVariant v((tjs_int64)w);
	(*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("width"), 0, &v, *dic);
	v = tTJSVariant((tjs_int64)h);
	(*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("height"), 0, &v, *dic);
	v = tTJSVariant((tjs_int)32);
	(*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("bpp"), 0, &v, *dic);
}

} // namespace

void TVPFateRegisterWebP()
{
	TVPRegisterGraphicLoadingHandler(ttstr(TJS_W(".webp")), (tTVPGraphicLoadingHandler)LoadWebP,
									 (tTVPGraphicHeaderLoadingHandler)LoadHeaderWebP,
									 (tTVPGraphicSaveHandler)NULL, (tTVPGraphicAcceptSaveHandler)NULL, (void *)NULL);
}
