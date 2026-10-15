/* SPDX-License-Identifier: MIT */

/**
 * @file
 * Native drawing helpers for Fate/stay night Realta Nua: drawLine(),
 * drawAALine() and Noise().
 *
 * Written from the interface description in docs/PLUGIN-INTERFACES.md (what
 * the game's scripts pass in and what they expect to see on the layer), using
 * textbook algorithms (Bresenham, Xiaolin Wu). No third-party plugin code was
 * consulted.
 *
 * All three operate on a Layer through its documented public properties
 * (mainImageBufferForWrite, mainImageBufferPitch, imageWidth, imageHeight) and
 * call Layer.update() when done.
 */

#include "tp_stub.h"

#include <stdint.h>
#include <algorithm>
#include <cmath>

namespace {

struct Surface
{
	iTJSDispatch2 *obj;
	uint8_t *base;
	tjs_int pitch;
	tjs_int w, h;
};

bool GetInt(iTJSDispatch2 *o, const tjs_char *name, tjs_int64 &out)
{
	tTJSVariant v;
	if (TJS_FAILED(o->PropGet(0, name, NULL, &v, o))) return false;
	out = (tjs_int64)v;
	return true;
}

bool OpenSurface(const tTJSVariant &arg, Surface &s)
{
	if (arg.Type() != tvtObject) return false;
	s.obj = arg.AsObjectNoAddRef();
	if (!s.obj) return false;
	tjs_int64 buf = 0, pitch = 0, w = 0, h = 0;
	if (!GetInt(s.obj, TJS_W("mainImageBufferForWrite"), buf) ||
		!GetInt(s.obj, TJS_W("mainImageBufferPitch"), pitch) ||
		!GetInt(s.obj, TJS_W("imageWidth"), w) ||
		!GetInt(s.obj, TJS_W("imageHeight"), h))
		return false;
	if (!buf || w <= 0 || h <= 0) return false;
	s.base = (uint8_t *)(intptr_t)buf;
	s.pitch = (tjs_int)pitch;
	s.w = (tjs_int)w;
	s.h = (tjs_int)h;
	return true;
}

void CloseSurface(Surface &s)
{
	tTJSVariant r;
	s.obj->FuncCall(0, TJS_W("update"), NULL, &r, 0, NULL, s.obj);
}

inline uint32_t *Px(const Surface &s, tjs_int x, tjs_int y)
{
	return (uint32_t *)(s.base + (ptrdiff_t)y * s.pitch) + x;
}

// Blend colour 'rgb' over the pixel with coverage 0..255.
inline void Blend(const Surface &s, tjs_int x, tjs_int y, uint32_t rgb, int cov)
{
	if (x < 0 || y < 0 || x >= s.w || y >= s.h || cov <= 0) return;
	uint32_t *p = Px(s, x, y);
	if (cov >= 255)
	{
		*p = 0xff000000u | rgb;
		return;
	}
	uint32_t d = *p;
	int da = d >> 24;
	int na = da + ((255 - da) * cov + 127) / 255;
	if (na <= 0) return;
	int wgt = (cov * 255 + na / 2) / na; // weight of the new colour
	if (wgt > 255) wgt = 255;
	uint32_t out = (uint32_t)na << 24;
	for (int sh = 0; sh <= 16; sh += 8)
	{
		int sc = (rgb >> sh) & 0xff, dc = (d >> sh) & 0xff;
		out |= (uint32_t)((dc * (255 - wgt) + sc * wgt + 127) / 255) << sh;
	}
	*p = out;
}

// layer, x1, y1, x2, y2, colour
bool LineArgs(tjs_int n, tTJSVariant **p, Surface &s, tjs_int &x1, tjs_int &y1,
			  tjs_int &x2, tjs_int &y2, uint32_t &rgb, int &alpha)
{
	if (n < 6 || !OpenSurface(*p[0], s)) return false;
	x1 = (tjs_int)*p[1];
	y1 = (tjs_int)*p[2];
	x2 = (tjs_int)*p[3];
	y2 = (tjs_int)*p[4];
	uint32_t c = (uint32_t)(tjs_int64)*p[5];
	rgb = c & 0xffffff;
	alpha = c >> 24;
	if (alpha == 0) alpha = 255; // colours given as plain 0xRRGGBB
	return true;
}

tjs_error DrawLine(tTJSVariant *result, tjs_int n, tTJSVariant **p, iTJSDispatch2 *)
{
	Surface s;
	tjs_int x1, y1, x2, y2;
	uint32_t rgb;
	int a;
	if (!LineArgs(n, p, s, x1, y1, x2, y2, rgb, a)) return TJS_E_BADPARAMCOUNT;
	tjs_int dx = std::abs(x2 - x1), dy = -std::abs(y2 - y1);
	tjs_int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
	tjs_int err = dx + dy;
	for (;;)
	{
		Blend(s, x1, y1, rgb, a);
		if (x1 == x2 && y1 == y2) break;
		tjs_int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x1 += sx; }
		if (e2 <= dx) { err += dx; y1 += sy; }
	}
	CloseSurface(s);
	if (result) result->Clear();
	return TJS_S_OK;
}

inline double Frac(double v) { return v - std::floor(v); }

tjs_error DrawAALine(tTJSVariant *result, tjs_int n, tTJSVariant **p, iTJSDispatch2 *)
{
	Surface s;
	tjs_int ix1, iy1, ix2, iy2;
	uint32_t rgb;
	int alpha;
	if (!LineArgs(n, p, s, ix1, iy1, ix2, iy2, rgb, alpha)) return TJS_E_BADPARAMCOUNT;
	double x0 = ix1, y0 = iy1, x1 = ix2, y1 = iy2;
	bool steep = std::fabs(y1 - y0) > std::fabs(x1 - x0);
	if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
	if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }
	double dx = x1 - x0, dy = y1 - y0;
	double grad = dx == 0.0 ? 1.0 : dy / dx;
	double y = y0;
	for (tjs_int x = (tjs_int)x0; x <= (tjs_int)x1; x++, y += grad)
	{
		tjs_int yi = (tjs_int)std::floor(y);
		double f = Frac(y);
		int c0 = (int)((1.0 - f) * alpha + 0.5), c1 = (int)(f * alpha + 0.5);
		if (steep)
		{
			Blend(s, yi, x, rgb, c0);
			Blend(s, yi + 1, x, rgb, c1);
		}
		else
		{
			Blend(s, x, yi, rgb, c0);
			Blend(s, x, yi + 1, rgb, c1);
		}
	}
	CloseSurface(s);
	if (result) result->Clear();
	return TJS_S_OK;
}

// Noise(%[ layer: Layer, monocro: bool ])  -> fills the layer with random pixels
tjs_error Noise(tTJSVariant *result, tjs_int n, tTJSVariant **p, iTJSDispatch2 *)
{
	if (n < 1 || p[0]->Type() != tvtObject) return TJS_E_BADPARAMCOUNT;
	iTJSDispatch2 *dic = p[0]->AsObjectNoAddRef();
	tTJSVariant lv, mv;
	if (TJS_FAILED(dic->PropGet(0, TJS_W("layer"), NULL, &lv, dic))) return TJS_E_INVALIDPARAM;
	bool mono = true;
	if (TJS_SUCCEEDED(dic->PropGet(0, TJS_W("monocro"), NULL, &mv, dic)) && mv.Type() != tvtVoid)
		mono = (tjs_int)mv != 0;
	Surface s;
	if (!OpenSurface(lv, s)) return TJS_E_INVALIDPARAM;
	uint32_t st = 0x2545F491u ^ (uint32_t)(uintptr_t)s.base;
	auto rnd = [&st]() { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
	for (tjs_int y = 0; y < s.h; y++)
	{
		uint32_t *row = Px(s, 0, y);
		for (tjs_int x = 0; x < s.w; x++)
		{
			uint32_t r = rnd();
			row[x] = mono ? (0xff000000u | ((r & 0xff) * 0x010101u))
						  : (0xff000000u | (r & 0xffffff));
		}
	}
	CloseSurface(s);
	if (result) result->Clear();
	return TJS_S_OK;
}

void Define(iTJSDispatch2 *global, const tjs_char *name, tTJSNativeClassMethodCallback cb)
{
	iTJSDispatch2 *fn = TJSCreateNativeClassMethod(cb);
	tTJSVariant v(fn);
	fn->Release();
	global->PropSet(TJS_MEMBERENSURE, name, NULL, &v, global);
}

} // namespace

void TVPFateRegisterDrawer()
{
	iTJSDispatch2 *g = TVPGetScriptDispatch();
	if (!g) return;
	Define(g, TJS_W("drawLine"), DrawLine);
	Define(g, TJS_W("drawAALine"), DrawAALine);
	g->Release();
}

void TVPFateRegisterNoise()
{
	iTJSDispatch2 *g = TVPGetScriptDispatch();
	if (!g) return;
	Define(g, TJS_W("Noise"), Noise);
	g->Release();
}
