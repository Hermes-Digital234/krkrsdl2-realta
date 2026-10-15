/* SPDX-License-Identifier: MIT */

/**
 * @file
 * Integrated replacements for the Windows plugin DLLs that Fate/stay night
 * Realta Nua (Kirikiri2 + Kirikiri Z patch) expects to find:
 *
 *   fstat.dll / dirlist.dll  -> Storages.fstat / dirlist / deleteFile /
 *                               moveFile / copyFile / createDirectory /
 *                               removeDirectory / isExistentDirectory,
 *                               and the global getDirList()
 *   KAGParser.dll            -> KAGParser class (vendored in ./kagparser)
 *   menu.dll                 -> MenuItem class (inert) and Window.menu
 *
 * The remaining plugins (util, resize, filter, drawer, firespark, slideopen,
 * wallpaper, kri18n, addFont ...) are provided as small TJS/native shims
 * installed when the game calls Plugins.link() (see below and
 * docs/PLUGIN-INTERFACES.md). Build with enable_external_plugins=false.
 */

#include "tp_stub.h"
#include "CharacterSet.h"
#include "kagparser/KAGParser.h"

void TVPFateRegisterDrawer();
void TVPFateRegisterNoise();

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

//---------------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------------
static bool FateToLocalPath(const tTJSVariant &v, std::string &out)
{
	ttstr name = v;
	name = TVPNormalizeStorageName(name);
	TVPGetLocalName(name);
	return TVPUtf16ToUtf8(out, name.AsStdString());
}
//---------------------------------------------------------------------------
static ttstr FateFromUtf8(const std::string &s)
{
	tjs_string w;
	TVPUtf8ToUtf16(w, s);
	return ttstr(w);
}
//---------------------------------------------------------------------------
static void FateStripTrailingSlash(std::string &p)
{
	while (p.size() > 1 && p[p.size() - 1] == '/')
		p.erase(p.size() - 1);
}
//---------------------------------------------------------------------------
static bool FateIsDir(const std::string &p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
//---------------------------------------------------------------------------
static bool FateMkdirs(const std::string &path)
{
	std::string p = path;
	FateStripTrailingSlash(p);
	if (p.empty()) return false;
	if (FateIsDir(p)) return true;
	size_t pos = 0;
	while ((pos = p.find('/', pos + 1)) != std::string::npos)
	{
		std::string sub = p.substr(0, pos);
		if (!FateIsDir(sub)) mkdir(sub.c_str(), 0755);
	}
	return mkdir(p.c_str(), 0755) == 0 || FateIsDir(p);
}
//---------------------------------------------------------------------------
static bool FateCopyFile(const std::string &src, const std::string &dst, bool failIfExists)
{
	if (failIfExists && access(dst.c_str(), F_OK) == 0) return false;
	FILE *in = fopen(src.c_str(), "rb");
	if (!in) return false;
	FILE *out = fopen(dst.c_str(), "wb");
	if (!out) { fclose(in); return false; }
	char buf[65536];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
	{
		if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
	}
	if (ferror(in)) ok = false;
	fclose(in);
	if (fclose(out) != 0) ok = false;
	if (!ok) unlink(dst.c_str());
	return ok;
}
//---------------------------------------------------------------------------
static void FateMakeDate(tTJSVariant &out, double ms)
{
	iTJSDispatch2 *global = TVPGetScriptDispatch();
	bool done = false;
	if (global)
	{
		tTJSVariant cls;
		if (TJS_SUCCEEDED(global->PropGet(0, TJS_W("Date"), NULL, &cls, global)) &&
			cls.Type() == tvtObject)
		{
			iTJSDispatch2 *clsobj = cls.AsObjectNoAddRef();
			iTJSDispatch2 *obj = NULL;
			if (TJS_SUCCEEDED(clsobj->CreateNew(0, NULL, NULL, &obj, 0, NULL, clsobj)) && obj)
			{
				tTJSVariant arg((tjs_int64)ms);
				tTJSVariant *args[1] = { &arg };
				if (TJS_SUCCEEDED(obj->FuncCall(0, TJS_W("setTime"), NULL, NULL, 1, args, obj)))
				{
					out = tTJSVariant(obj, obj);
					done = true;
				}
				obj->Release();
			}
		}
		global->Release();
	}
	if (!done) out = tTJSVariant(ms);
}
//---------------------------------------------------------------------------
static void FateDictSet(iTJSDispatch2 *dic, const tjs_char *name, const tTJSVariant &v)
{
	tTJSVariant tmp = v;
	dic->PropSet(TJS_MEMBERENSURE, name, NULL, &tmp, dic);
}
//---------------------------------------------------------------------------
// Storages.fstat(path) -> %[size, mtime, atime, ctime]
static tjs_error FateFstat(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	std::string path;
	if (!FateToLocalPath(*param[0], path)) return TJS_E_FAIL;
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
	{
		ttstr msg = TJS_W("cannot open : ");
		msg += ttstr(*param[0]);
		TVPThrowExceptionMessage(msg.c_str());
	}
	iTJSDispatch2 *dic = TJSCreateDictionaryObject();
	try
	{
		FateDictSet(dic, TJS_W("size"), tTJSVariant((tjs_int64)st.st_size));
		tTJSVariant d;
		FateMakeDate(d, (double)st.st_mtime * 1000.0); FateDictSet(dic, TJS_W("mtime"), d);
		FateMakeDate(d, (double)st.st_atime * 1000.0); FateDictSet(dic, TJS_W("atime"), d);
		FateMakeDate(d, (double)st.st_ctime * 1000.0); FateDictSet(dic, TJS_W("ctime"), d);
		if (result) *result = tTJSVariant(dic, dic);
	}
	catch (...)
	{
		dic->Release();
		throw;
	}
	dic->Release();
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
// Storages.dirlist(dir) / getDirList(dir) -> [ "name", "subdir/", ... ]
static tjs_error FateDirList(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	std::string dir;
	if (!FateToLocalPath(*param[0], dir)) return TJS_E_FAIL;
	FateStripTrailingSlash(dir);
	if (dir.empty()) dir = "/";

	DIR *dp = opendir(dir.c_str());
	if (!dp) TVPThrowExceptionMessage(TJS_W("Directory not found."));

	std::vector<std::string> names;
	struct dirent *ent;
	while ((ent = readdir(dp)) != NULL)
	{
		std::string n = ent->d_name;
		if (n == "." || n == "..") continue;
		std::string full = dir + (dir == "/" ? "" : "/") + n;
		if (FateIsDir(full)) n += "/";
		names.push_back(n);
	}
	closedir(dp);
	std::sort(names.begin(), names.end());

	iTJSDispatch2 *array = TJSCreateArrayObject();
	try
	{
		for (size_t i = 0; i < names.size(); i++)
		{
			tTJSVariant v = FateFromUtf8(names[i]);
			array->PropSetByNum(TJS_MEMBERENSURE, (tjs_int)i, &v, array);
		}
		if (result) *result = tTJSVariant(array, array);
	}
	catch (...)
	{
		array->Release();
		throw;
	}
	array->Release();
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
// diagnostics: append one line to ./fate-debug.log (the engine's own log file is
// only written with -forcelog). Compiled in with -DOPTION_FATE_DEBUG_LOG=ON (the default);
// otherwise this does nothing.
void TVPFateDebugLog(const char *line)
{
#ifdef KRKRSDL2_FATE_DEBUG_LOG
	FILE *f = fopen("fate-debug.log", "ab");
	if (!f) return;
	fputs(line, f);
	fputc('\n', f);
	fclose(f);
#else
	(void)line;
#endif
}
extern bool TVPFateGetVirtualCursor(int *x, int *y);
// returns (y << 16) | x while the engine emulates a pointer warp, -1 while it does not,
// -2 when the feature is not compiled in (-DOPTION_FATE_VIRTUAL_CURSOR=ON)
static tjs_error FateVirtualCursorProc(tTJSVariant *result, tjs_int, tTJSVariant **, iTJSDispatch2 *)
{
#ifdef KRKRSDL2_FATE_VCURSOR
	int x = 0, y = 0;
	if (result)
		*result = (tjs_int)(TVPFateGetVirtualCursor(&x, &y) ? (((y & 0x7fff) << 16) | (x & 0xffff)) : -1);
#else
	if (result)
		*result = (tjs_int)-2;
#endif
	return TJS_S_OK;
}
static tjs_error FateDebugLogProc(tTJSVariant *, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	ttstr t(*param[0]);
	std::string u;
	{
		std::vector<char> b(t.GetLen() * 4 + 4);
		tjs_int n = TVPWideCharToUtf8String(t.c_str(), &b[0]);
		b[n] = 0;
		u = &b[0];
	}
	TVPFateDebugLog(u.c_str());
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
static tjs_error FateDeleteFile(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	std::string p;
	bool ok = FateToLocalPath(*param[0], p) && unlink(p.c_str()) == 0;
	TVPClearStorageCaches();
	if (result) *result = (tjs_int)(ok ? 1 : 0);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
static tjs_error FateMoveFile(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 2) return TJS_E_BADPARAMCOUNT;
	std::string a, b;
	bool ok = FateToLocalPath(*param[0], a) && FateToLocalPath(*param[1], b);
	if (ok)
	{
		ok = rename(a.c_str(), b.c_str()) == 0;
		if (!ok && errno == EXDEV)
			ok = FateCopyFile(a, b, false) && unlink(a.c_str()) == 0;
	}
	TVPClearStorageCaches();
	if (result) *result = (tjs_int)(ok ? 1 : 0);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
static tjs_error FateCopyFileProc(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 2) return TJS_E_BADPARAMCOUNT;
	std::string a, b;
	bool failIfExists = numparams >= 3 && (tjs_int)*param[2] != 0;
	bool ok = FateToLocalPath(*param[0], a) && FateToLocalPath(*param[1], b) &&
		FateCopyFile(a, b, failIfExists);
	TVPClearStorageCaches();
	if (result) *result = (tjs_int)(ok ? 1 : 0);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
static tjs_error FateCreateDirectory(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	std::string p;
	bool ok = FateToLocalPath(*param[0], p) && FateMkdirs(p);
	TVPClearStorageCaches();
	if (result) *result = (tjs_int)(ok ? 1 : 0);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
static tjs_error FateRemoveDirectory(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	std::string p;
	bool ok = FateToLocalPath(*param[0], p);
	if (ok) { FateStripTrailingSlash(p); ok = rmdir(p.c_str()) == 0; }
	TVPClearStorageCaches();
	if (result) *result = (tjs_int)(ok ? 1 : 0);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
static tjs_error FateIsExistentDirectory(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	std::string p;
	bool ok = FateToLocalPath(*param[0], p);
	if (ok) { FateStripTrailingSlash(p); ok = FateIsDir(p); }
	if (result) *result = (tjs_int)(ok ? 1 : 0);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
// Storages.fateLoadMo(storage) -> %[ "ctx\x04msgid" => "msgstr", ... ] or void
// (GNU gettext .mo reader; the original kri18n.dll did this natively)
//---------------------------------------------------------------------------
extern bool TVPFateReadStorage(const ttstr &name, std::vector<unsigned char> &out);

static tjs_error FateLoadMo(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	if (result) result->Clear();
	std::vector<unsigned char> d;
	if (!TVPFateReadStorage(ttstr(*param[0]), d) || d.size() < 28) return TJS_S_OK;
	const unsigned char *b = &d[0];
	bool le;
	tjs_uint32 magic = (tjs_uint32)b[0] | ((tjs_uint32)b[1] << 8) | ((tjs_uint32)b[2] << 16) | ((tjs_uint32)b[3] << 24);
	if (magic == 0x950412deU) le = true;
	else if (magic == 0xde120495U) le = false;
	else return TJS_S_OK;
	#define MO_U32(off) (le ? ((tjs_uint32)b[(off)] | ((tjs_uint32)b[(off)+1] << 8) | ((tjs_uint32)b[(off)+2] << 16) | ((tjs_uint32)b[(off)+3] << 24)) \
	                        : ((tjs_uint32)b[(off)+3] | ((tjs_uint32)b[(off)+2] << 8) | ((tjs_uint32)b[(off)+1] << 16) | ((tjs_uint32)b[(off)] << 24)))
	tjs_uint32 n = MO_U32(8), otab = MO_U32(12), ttab = MO_U32(16);
	if ((tjs_uint64)otab + (tjs_uint64)n * 8 > d.size() || (tjs_uint64)ttab + (tjs_uint64)n * 8 > d.size()) return TJS_S_OK;

	iTJSDispatch2 *dic = TJSCreateDictionaryObject();
	try
	{
		for (tjs_uint32 i = 0; i < n; i++)
		{
			tjs_uint32 ol = MO_U32(otab + i * 8), oo = MO_U32(otab + i * 8 + 4);
			tjs_uint32 tl = MO_U32(ttab + i * 8), to = MO_U32(ttab + i * 8 + 4);
			if ((tjs_uint64)oo + ol > d.size() || (tjs_uint64)to + tl > d.size()) continue;
			std::string o((const char *)b + oo, ol), t((const char *)b + to, tl);
			size_t z;
			if ((z = o.find('\0')) != std::string::npos) o.resize(z); // plural: first form
			if ((z = t.find('\0')) != std::string::npos) t.resize(z);
			if (o.empty() || t.empty()) continue; // header / untranslated
			if (o.find('\x04') == std::string::npos) o = std::string("\x04") + o; // no context
			tjs_string wo, wt;
			TVPUtf8ToUtf16(wo, o);
			TVPUtf8ToUtf16(wt, t);
			FateDictSet(dic, wo.c_str(), tTJSVariant(ttstr(wt)));
		}
		if (result) *result = tTJSVariant(dic, dic);
	}
	catch (...)
	{
		dic->Release();
		throw;
	}
	dic->Release();
	#undef MO_U32
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
// registration of the methods above on the Storages class
//---------------------------------------------------------------------------
static void FateAddStatic(tTJSNativeClass *cls, const tjs_char *name,
	tjs_error (TJS_INTF_METHOD *proc)(tTJSVariant *, tjs_int, tTJSVariant **, iTJSDispatch2 *))
{
	TJSNativeClassRegisterNCM(cls, name, TJSCreateNativeClassMethod(proc),
		cls->GetClassName().c_str(), nitMethod, TJS_STATICMEMBER);
}
//---------------------------------------------------------------------------
void TVPFateCompatAddStorageMembers(tTJSNativeClass *cls)
{
	FateAddStatic(cls, TJS_W("fstat"), FateFstat);
	FateAddStatic(cls, TJS_W("dirlist"), FateDirList);
	FateAddStatic(cls, TJS_W("deleteFile"), FateDeleteFile);
	FateAddStatic(cls, TJS_W("moveFile"), FateMoveFile);
	FateAddStatic(cls, TJS_W("copyFile"), FateCopyFileProc);
	FateAddStatic(cls, TJS_W("createDirectory"), FateCreateDirectory);
	FateAddStatic(cls, TJS_W("removeDirectory"), FateRemoveDirectory);
	FateAddStatic(cls, TJS_W("isExistentDirectory"), FateIsExistentDirectory);
	FateAddStatic(cls, TJS_W("fateLoadMo"), FateLoadMo);
	FateAddStatic(cls, TJS_W("fateDebugLog"), FateDebugLogProc);
	FateAddStatic(cls, TJS_W("fateVirtualCursor"), FateVirtualCursorProc);
}
//---------------------------------------------------------------------------
// Window.menu : lazily creates an inert root MenuItem per window
//---------------------------------------------------------------------------
void TVPFateCompatAddWindowMembers(tTJSNativeClass *cls)
{
	TJS_BEGIN_NATIVE_PROP_DECL(menu)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			tTJSVariant root;
			if (TJS_SUCCEEDED(objthis->PropGet(0, TJS_W("__fateMenuRoot"), NULL, &root, objthis)) &&
				root.Type() == tvtObject)
			{
				*result = root;
				return TJS_S_OK;
			}
			iTJSDispatch2 *global = TVPGetScriptDispatch();
			if (!global) return TJS_E_FAIL;
			tTJSVariant mi;
			tjs_error er = global->PropGet(0, TJS_W("MenuItem"), NULL, &mi, global);
			global->Release();
			if (TJS_FAILED(er) || mi.Type() != tvtObject) return TJS_E_FAIL;
			iTJSDispatch2 *miobj = mi.AsObjectNoAddRef();
			tTJSVariant win(objthis, objthis);
			tTJSVariant *args[1] = { &win };
			iTJSDispatch2 *obj = NULL;
			er = miobj->CreateNew(0, NULL, NULL, &obj, 1, args, miobj);
			if (TJS_FAILED(er) || !obj) return TJS_E_FAIL;
			tTJSVariant v(obj, obj);
			obj->Release();
			objthis->PropSet(TJS_MEMBERENSURE, TJS_W("__fateMenuRoot"), NULL, &v, objthis);
			// draw it as a menu bar overlay (see FateMenuBarScript)
			try
			{
				iTJSDispatch2 *g2 = TVPGetScriptDispatch();
				if (g2)
				{
					tTJSVariant *a2[2] = { &win, &v };
					g2->FuncCall(0, TJS_W("fateMenuBarAttach"), NULL, NULL, 2, a2, g2);
					g2->Release();
				}
			}
			catch (...) {}
			*result = v;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL_OUTER(cls, menu)
}
//---------------------------------------------------------------------------
// Scripts.getObjectKeys(obj) -> [ member names ]   (formerly scriptsEx.dll)
static tjs_error FateGetObjectKeys(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	iTJSDispatch2 *array = TJSCreateArrayObject();
	try
	{
		if (param[0]->Type() == tvtObject && param[0]->AsObjectNoAddRef())
		{
			struct KeyCollector : public tTJSDispatch
			{
				iTJSDispatch2 *Arr;
				tjs_int Idx;
				KeyCollector(iTJSDispatch2 *a) : Arr(a), Idx(0) {}
				tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32, const tjs_char *, tjs_uint32 *,
					tTJSVariant *res, tjs_int np, tTJSVariant **p, iTJSDispatch2 *)
				{
					if (np < 3) return TJS_E_BADPARAMCOUNT;
					tjs_uint32 flags = (tjs_int)*p[1];
					if (!(flags & TJS_HIDDENMEMBER))
					{
						tTJSVariant name = *p[0];
						Arr->PropSetByNum(TJS_MEMBERENSURE, Idx++, &name, Arr);
					}
					if (res) *res = (tjs_int)1;
					return TJS_S_OK;
				}
			} collector(array);
			tTJSVariantClosure clo(&collector, NULL);
			iTJSDispatch2 *obj = param[0]->AsObjectNoAddRef();
			obj->EnumMembers(TJS_IGNOREPROP, &clo, obj);
		}
		if (result) *result = tTJSVariant(array, array);
	}
	catch (...)
	{
		array->Release();
		throw;
	}
	array->Release();
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
// System font discovery
//
// krkrsdl2 has no system font enumeration on Linux, so any face that is not
// bundled with the game ("ＭＳ ゴシック" here) cannot be found, and the
// fallback face (-deffont, "Noto Sans CJK JP") fails too. Scan the usual font
// directories and register every font with FreeType. If the default face is
// not among them, pick a CJK-capable one.
//---------------------------------------------------------------------------
extern void TVPAddSystemFontToFreeType(const std::string &storage, std::vector<tjs_string> *faces);
extern const tjs_char *TVPGetDefaultFontName();
extern void TVPSetDefaultFontName(const tjs_char *name);

static bool FateHasFontExt(const std::string &n)
{
	size_t dot = n.find_last_of('.');
	if (dot == std::string::npos) return false;
	std::string e = n.substr(dot + 1);
	for (size_t i = 0; i < e.size(); i++)
		if (e[i] >= 'A' && e[i] <= 'Z') e[i] = (char)(e[i] - 'A' + 'a');
	return e == "ttf" || e == "otf" || e == "ttc" || e == "otc";
}

static void FateScanFontDir(const std::string &dir, int depth, std::vector<std::string> &files)
{
	if (depth > 8) return;
	DIR *dp = opendir(dir.c_str());
	if (!dp) return;
	std::vector<std::string> names;
	struct dirent *ent;
	while ((ent = readdir(dp)) != NULL)
	{
		std::string n = ent->d_name;
		if (n == "." || n == "..") continue;
		names.push_back(n);
	}
	closedir(dp);
	std::sort(names.begin(), names.end());
	for (size_t i = 0; i < names.size(); i++)
	{
		std::string full = dir + "/" + names[i];
		struct stat st;
		if (stat(full.c_str(), &st) != 0) continue; // follows symlinks
		if (S_ISDIR(st.st_mode)) FateScanFontDir(full, depth + 1, files);
		else if (S_ISREG(st.st_mode) && FateHasFontExt(names[i])) files.push_back(full);
	}
}

static void FateLoadSystemFonts()
{
	std::vector<std::string> dirs;
	const char *home = getenv("HOME");
	const char *xdg = getenv("XDG_DATA_HOME");
	dirs.push_back("/usr/share/fonts");
	dirs.push_back("/usr/local/share/fonts");
	dirs.push_back("/System/Library/Fonts");      // macOS
	dirs.push_back("/Library/Fonts");
	if (xdg && *xdg) dirs.push_back(std::string(xdg) + "/fonts");
	if (home && *home)
	{
		dirs.push_back(std::string(home) + "/.local/share/fonts");
		dirs.push_back(std::string(home) + "/.fonts");
		dirs.push_back(std::string(home) + "/Library/Fonts");   // macOS
	}
	dirs.push_back("./fonts"); // a "fonts" folder next to the game

	std::vector<std::string> files;
	for (size_t i = 0; i < dirs.size(); i++) FateScanFontDir(dirs[i], 0, files);

	// Several weights of one family (Light, Medium, Black, ...) all register as
	// regular style under the same name and the first one registered wins.
	// Register the plain/regular files first so text does not come out heavy.
	{
		std::vector<std::pair<int, std::string> > ranked;
		for (size_t i = 0; i < files.size(); i++)
		{
			std::string b = files[i];
			size_t sl = b.find_last_of('/');
			if (sl != std::string::npos) b = b.substr(sl + 1);
			for (size_t k = 0; k < b.size(); k++) if (b[k] >= 'A' && b[k] <= 'Z') b[k] = (char)(b[k] - 'A' + 'a');
			int score = 1;
			if (b.find("regular") != std::string::npos || b.find("-book") != std::string::npos) score = 0;
			else if (b.find("demilight") != std::string::npos || b.find("medium") != std::string::npos) score = 2;
			else if (b.find("thin") != std::string::npos || b.find("extralight") != std::string::npos) score = 3;
			else if (b.find("light") != std::string::npos) score = 2;
			else if (b.find("semibold") != std::string::npos) score = 4;
			else if (b.find("bold") != std::string::npos || b.find("black") != std::string::npos || b.find("heavy") != std::string::npos) score = 5;
			ranked.push_back(std::make_pair(score, files[i]));
		}
		std::stable_sort(ranked.begin(), ranked.end(),
			[](const std::pair<int, std::string> &a, const std::pair<int, std::string> &b) { return a.first < b.first; });
		for (size_t i = 0; i < ranked.size(); i++) files[i] = ranked[i].second;
	}

	std::vector<tjs_string> faces;
	for (size_t i = 0; i < files.size(); i++)
	{
		try { TVPAddSystemFontToFreeType(files[i], &faces); }
		catch (...) { /* unreadable font: ignore */ }
	}
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "fate_compat: %d font files scanned, %d faces registered", (int)files.size(), (int)faces.size());
		tjs_string w; TVPUtf8ToUtf16(w, std::string(buf));
		TVPAddLog(ttstr(w));
	}
	if (faces.empty())
	{
		TVPAddLog(TJS_W("fate_compat: NO fonts found. Install a Japanese font (Arch: pacman -S noto-fonts-cjk) or put a .ttf/.otf in ./fonts next to the game"));
		return;
	}

	// Make sure the default face exists; otherwise choose a CJK-capable one.
	tjs_string cur(TVPGetDefaultFontName());
	if (std::find(faces.begin(), faces.end(), cur) != faces.end()) return;
	static const tjs_char *prefs[] = {
		TJS_W("Noto Sans CJK JP"), TJS_W("Noto Sans JP"), TJS_W("Source Han Sans JP"),
		TJS_W("Source Han Sans"), TJS_W("IPAexGothic"), TJS_W("IPAGothic"),
		TJS_W("TakaoPGothic"), TJS_W("TakaoGothic"), TJS_W("VL Gothic"),
		TJS_W("VL PGothic"), TJS_W("Sazanami Gothic"), TJS_W("Noto Serif CJK JP"),
		TJS_W("Noto Sans CJK SC"), TJS_W("WenQuanYi Zen Hei"), TJS_W("WenQuanYi Micro Hei"),
		TJS_W("Droid Sans Fallback"), TJS_W("Unifont"), NULL };
	for (int i = 0; prefs[i]; i++)
	{
		if (std::find(faces.begin(), faces.end(), tjs_string(prefs[i])) != faces.end())
		{
			TVPSetDefaultFontName(prefs[i]);
			TVPAddLog(ttstr(TJS_W("fate_compat: default font set to ")) + ttstr(prefs[i]));
			return;
		}
	}
	// no known Japanese font: use any face that loaded so it renders instead of throwing
	TVPSetDefaultFontName(faces[0].c_str());
	TVPAddLog(ttstr(TJS_W("fate_compat: no Japanese font installed; using ")) + ttstr(faces[0]) +
		ttstr(TJS_W(" (Japanese text will be missing - install noto-fonts-cjk)")));
}
//---------------------------------------------------------------------------
// script-level pieces, installed once the script engine is up
// its all in quotes as its put in the engines scripting language, its not compiled
//---------------------------------------------------------------------------
static const char FateMenuBarScript[] = R"TJS(
// ---------------------------------------------------------------------------
// FateMenuBar: draws a classic menu bar (and its drop-down menus) on top of a
// window with Layers, replacing the native Win32 menu bar of menu.dll.
//   * the bar appears when the mouse touches the top edge of the window
//   * F10 pins/unpins it
//   * it is hidden while root.visible == false (the game does this in fullscreen)
// Hit-testing is done by the engine: every item is its own transparent layer, so
// no mouse-coordinate arithmetic is needed.
// ---------------------------------------------------------------------------
class FateHitLayer extends Layer
{
	var owner, arg;
	function FateHitLayer(win, parent, own, a) { super.Layer(win, parent); owner = own; arg = a; }
	function onMouseEnter() { owner.hitEnter(arg); }
	function onMouseLeave() { owner.hitLeave(arg); }
	function onMouseDown(x, y, button, shift) { owner.hitDown(arg, button); }
	function onMouseUp(x, y, button, shift) { }
}

class FateMenuBar
{
	var win, root, pl, timer, rtimer;
	var vis, catcher;          // visual layer / full-size click catcher
	var strip;                 // hot strip along the top edge
	var hits = [];             // transient hit layers
	var pinned = false, hover = false, inside = false, rebuiltAt = 0, actAt = 0;
	var levels = [];           // open drop-downs: %[ item, x, y, w, h, rows, hot ]
	var barItems = [];         // %[ item, x, w ]
	var hotBar = -1;
	var lastKey = "";
	var ready = false;
	var absBase = 0x7fff0000; // leaves room for the hit layers: absolute is a 32-bit int and wrapped negative near INT_MAX

	function FateMenuBar(w, r)
	{
		win = w; root = r;
		timer = new Timer(this, "onTick");
		timer.interval = 150;
		timer.enabled = true;
		rtimer = new Timer(this, "onRebuild");
		rtimer.interval = 10;
		rtimer.enabled = false;
	}
	function finalize()
	{
		try { timer.enabled = false; rtimer.enabled = false; invalidate timer; invalidate rtimer; } catch(e) {}
		clearHits();
		try { if(vis !== void) invalidate vis; if(catcher !== void) invalidate catcher; if(strip !== void) invalidate strip; } catch(e) {}
	}

	// on-screen scale of the layer tree (window may be bigger than the layer)
	property scale { getter() {
		if(pl === void || pl === null) return 1.0;
		var iw = win.innerWidth, ih = win.innerHeight, pw = pl.width, ph = pl.height;
		if(iw <= 0 || ih <= 0 || pw <= 0 || ph <= 0) return 1.0;
		var s = Math.min(iw / pw, ih / ph);
		if(s < 0.5) s = 0.5; else if(s > 4.0) s = 4.0;
		return s;
	} }
	function S(v) { return int(Math.round(v / scale)); }
	property barH { getter() { return S(22); } }
	property fontH { getter() { return S(14); } }

	function isVis(item) { return item.visible !== false; }
	function cleanCaption(c) {
		var s = "", i = 0;
		c = string c;
		while(i < c.length) {
			var ch = c[i];
			if(ch == "&") { i++; if(i < c.length) s += c[i]; }
			else s += ch;
			i++;
		}
		return s;
	}
	function visibleChildren(item) {
		var r = [], c = item.children;
		for(var i = 0; i < c.count; i++) if(isVis(c[i])) r.add(c[i]);
		return r;
	}
	property wanted { getter() {
		if(visibleChildren(root).count <= 0) return false;
		if(root.visible !== false) return true;
		// In fullscreen the game shows the menu only while the pointer is on the top row (y == 0) and hides
		// it as soon as the pointer is lower, relying on Windows' menu loop to keep a menu open. This overlay
		// has its own hover logic, so there it only honours the game when the menu bar is really disabled.
		if(win.fullScreen) return (typeof win.menuBarEnable == "undefined") ? true : (win.menuBarEnable != false);
		return false;
	} }
	property popupOpen { getter() { return levels.count > 0; } }
	property barShown { getter() { return wanted && (pinned || hover || popupOpen); } }

	function onTick()
	{
		var p = win.primaryLayer;
		if(p === void || p === null) return;
		if(!ready) {
			pl = p;
			vis = new Layer(win, pl);
			vis.type = ltAlpha;
			vis.enabled = false;           // never takes mouse events itself
			vis.visible = false;
			catcher = new FateHitLayer(win, pl, this, %[ kind:"catcher" ]);
			catcher.visible = false;
			strip = new FateHitLayer(win, pl, this, %[ kind:"strip" ]);
			strip.visible = false;
			ready = true;
			layout(true);
			return;
		}
		if(hover && !inside && !popupOpen && !pinned && System.getTickCount() - actAt > 500) { hover = false; layout(true); return; }
		layout(false);
	}

	function schedule() { rtimer.enabled = true; }
	function onRebuild() { rtimer.enabled = false; rebuild(); }

	function layout(force)
	{
		if(!ready) return;
		var key = pl.width + "x" + pl.height + "@" + scale + (barShown ? "1" : "0");
		if(wanted) {
			var kids = visibleChildren(root);
			for(var i = 0; i < kids.count; i++) key += "|" + kids[i].caption + (kids[i].enabled === false ? "x" : "");
		} else key += "-";
		if(!force && key == lastKey) return;
		lastKey = key;
		rebuild();
	}

	function clearHits()
	{
		for(var i = 0; i < hits.count; i++) { try { invalidate hits[i]; } catch(e) {} }
		hits.clear();
	}
	function addHit(l, t, w, h, arg)
	{
		var hl = new FateHitLayer(win, pl, this, arg);
		hl.setPos(l, t, w, h);
		hl.fillRect(0, 0, w, h, 0x00000000);
		hl.hitThreshold = 0;
		hl.absolute = absBase + 3 + hits.count;
		hl.visible = true;
		hits.add(hl);
		return hl;
	}

	function rebuild()
	{
		if(!ready) return;
		clearHits();
		rebuiltAt = System.getTickCount();
		var w = pl.width, h = pl.height;
		dbg("rebuild layer " + w + "x" + h + " window " + win.innerWidth + "x" + win.innerHeight + " scale " + scale + " barH " + barH + " shown " + barShown + " hover " + hover);
		if(!wanted) {
			vis.visible = false; catcher.visible = false; strip.visible = false;
			levels.clear(); hover = false; inside = false; return;
		}
		if(!barShown) {
			vis.visible = false; catcher.visible = false;
			strip.setPos(0, 0, w, S(4));
			strip.fillRect(0, 0, w, S(4), 0x00000000);
			strip.hitThreshold = 0;
			strip.absolute = absBase + 2;
			strip.visible = true;
			return;
		}
		strip.visible = false;
		var bh = barH;
		vis.setPos(0, 0, w, popupOpen ? h : bh);
		vis.absolute = absBase + 1;
		vis.visible = true;
		if(popupOpen) {
			catcher.setPos(0, 0, w, h);
			catcher.fillRect(0, 0, w, h, 0x00000000);
			catcher.hitThreshold = 0;
			catcher.absolute = absBase;
			catcher.visible = true;
		} else catcher.visible = false;
		redraw();
		addHit(0, 0, w, bh, %[ kind:"bar" ]);
		for(var i = 0; i < barItems.count; i++) {
			var b = barItems[i];
			addHit(b.x, 0, b.w, bh - 1, %[ kind:"top", i:i ]);
		}
		for(var l = 0; l < levels.count; l++) {
			var lv = levels[l];
			addHit(lv.x, lv.y, lv.w, lv.h, %[ kind:"panel" ]);
			for(var r = 0; r < lv.rows.count; r++) {
				var row = lv.rows[r];
				addHit(lv.x, lv.y + row.y, lv.w, row.h, %[ kind:"row", l:l, r:r ]);
			}
		}
	}

	// draws bar + drop-downs into the visual layer (also called for hover changes)
	function redraw()
	{
		var w = vis.width, bh = barH;
		vis.face = dfAlpha;
		vis.fillRect(0, 0, w, vis.height, 0x00000000);
		vis.font.height = fontH;
		vis.fillRect(0, 0, w, bh, 0xfff0f0f0);
		vis.fillRect(0, bh - 1, w, 1, 0xffa0a0a0);
		barItems.clear();
		var kids = visibleChildren(root), x = 0, pad = S(10);
		for(var i = 0; i < kids.count; i++) {
			var cap = cleanCaption(kids[i].caption);
			var iw = vis.font.getTextWidth(cap) + pad * 2;
			if(i == hotBar && popupOpen) vis.fillRect(x, 0, iw, bh - 1, 0xff99ccee);
			else if(i == hotBar) vis.fillRect(x, 0, iw, bh - 1, 0xffd8e8f4);
			vis.drawText(x + pad, int((bh - fontH) / 2) - 1, cap, kids[i].enabled === false ? 0x888888 : 0x000000, 255, true);
			barItems.add(%[ item:kids[i], x:x, w:iw ]);
			x += iw;
		}
		for(var l = 0; l < levels.count; l++) drawLevel(levels[l]);
	}

	function buildLevel(item, x, y)
	{
		var kids = visibleChildren(item);
		var rowH = S(22), sepH = S(8), pad = S(6);
		vis.font.height = fontH;
		var maxw = 0, rows = [];
		for(var i = 0; i < kids.count; i++) {
			var it = kids[i], cap = cleanCaption(it.caption);
			var isSep = (cap == "-");
			var sc = (typeof it.shortcut == "String") ? it.shortcut : "";
			var tw = isSep ? 0 : vis.font.getTextWidth(cap) + (sc != "" ? S(24) + vis.font.getTextWidth(sc) : 0);
			if(tw > maxw) maxw = tw;
			rows.add(%[ item:it, cap:cap, sep:isSep, sc:sc ]);
		}
		var w = maxw + S(34) + S(26);
		var h = pad;
		for(var i = 0; i < rows.count; i++) { rows[i].y = h; rows[i].h = rows[i].sep ? sepH : rowH; h += rows[i].h; }
		h += pad;
		if(x + w > pl.width) x = Math.max(0, pl.width - w);
		if(y + h > pl.height) y = Math.max(0, pl.height - h);
		return %[ item:item, x:x, y:y, w:w, h:h, rows:rows, hot:-1 ];
	}

	function drawLevel(lv)
	{
		var x = lv.x, y = lv.y, w = lv.w, h = lv.h;
		vis.fillRect(x, y, w, h, 0xffa0a0a0);
		vis.fillRect(x + 1, y + 1, w - 2, h - 2, 0xfff5f5f5);
		for(var i = 0; i < lv.rows.count; i++) {
			var r = lv.rows[i], it = r.item, ry = y + r.y;
			if(r.sep) { vis.fillRect(x + S(30), ry + int(r.h / 2), w - S(36), 1, 0xffc8c8c8); continue; }
			var enabled = (it.enabled !== false);
			if(i == lv.hot && enabled) vis.fillRect(x + 2, ry, w - 4, r.h, 0xffcce4f7);
			var col = enabled ? 0x000000 : 0x909090;
			if(it.checked) {
				var m = S(6), cx = x + S(10), cy = ry + int((r.h - m) / 2);
				if(it.radio) vis.fillRect(cx + 1, cy + 1, m - 2, m - 2, 0xff000000 | col);
				else vis.fillRect(cx, cy, m, m, 0xff000000 | col);
			}
			vis.drawText(x + S(30), ry + int((r.h - fontH) / 2) - 1, r.cap, col, 255, true);
			if(r.sc != "") {
				var sw = vis.font.getTextWidth(r.sc);
				vis.drawText(x + w - S(26) - sw, ry + int((r.h - fontH) / 2) - 1, r.sc, col, 255, true);
			}
			if(visibleChildren(it).count > 0) {
				var ax = x + w - S(14), ay = ry + int(r.h / 2), a = S(4);
				for(var k = 0; k < a; k++) vis.fillRect(ax + k, ay - (a - k) + 1, 1, (a - k) * 2 - 1, 0xff000000 | col);
			}
		}
	}

	// ---- events (from FateHitLayer) -----------------------------------
	function openTop(i)
	{
		levels.clear();
		hotBar = i;
		if(i >= 0) {
			var b = barItems[i];
			if(visibleChildren(b.item).count > 0) levels.add(buildLevel(b.item, b.x, barH - 1));
		}
		schedule();
	}
	function closeAll()
	{
		levels.clear(); hotBar = -1;
		if(!pinned) hover = false;
		schedule();
	}

	var dbgN = 0;
	function dbg(m) { if(dbgN < 120) { dbgN++; Storages.fateDebugLog("menubar: " + m); } }
	function hitEnter(a)
	{
		inside = true; actAt = System.getTickCount();
		dbg("enter " + a.kind + (a.i !== void ? " i=" + a.i : "") + (a.r !== void ? " l=" + a.l + " r=" + a.r : "") + " cursor " + pl.cursorX + "," + pl.cursorY);
		switch(a.kind) {
		case "strip":
			if(!hover) { hover = true; schedule(); }
			break;
		case "top":
			if(popupOpen && hotBar != a.i) openTop(a.i);
			else if(hotBar != a.i) { hotBar = a.i; redraw(); }
			break;
		case "row":
			var lv = levels[a.l];
			if(lv === void) break;
			var row = lv.rows[a.r];
			var wantSub = (!row.sep && row.item.enabled !== false && visibleChildren(row.item).count > 0);
			var cur = (levels.count > a.l + 1) ? levels[a.l + 1] : void;
			if(lv.hot == a.r && ((cur === void && !wantSub) || (cur !== void && wantSub && cur.item === row.item))) break;
			lv.hot = a.r;
			if(cur !== void && wantSub && cur.item === row.item) { redraw(); break; }
			var structural = false;
			while(levels.count > a.l + 1) { levels.erase(levels.count - 1); structural = true; }
			if(wantSub) {
				levels.add(buildLevel(row.item, lv.x + lv.w - 2, lv.y + row.y - S(6)));
				structural = true;
			}
			if(structural) schedule(); else redraw();
			break;
		}
	}
	function hitLeave(a)
	{
		dbg("leave " + a.kind + (a.i !== void ? " i=" + a.i : "") + " cursor " + pl.cursorX + "," + pl.cursorY);
		if(System.getTickCount() - rebuiltAt > 250) inside = false;
		if(System.getTickCount() - rebuiltAt < 250) return;
		if(a.kind == "row") {
			var lv = levels[a.l];
			if(lv !== void && lv.hot == a.r) { lv.hot = -1; redraw(); }
		} else if(a.kind == "top" && !popupOpen && hotBar == a.i) { hotBar = -1; redraw(); }
	}
	function hitDown(a, button)
	{
		inside = true; actAt = System.getTickCount();
		if(a.kind == "catcher") { closeAll(); return; }
		if(button != mbLeft) return;
		if(a.kind == "top") {
			var it = barItems[a.i].item;
			if(visibleChildren(it).count > 0) {
				if(popupOpen && hotBar == a.i) { levels.clear(); schedule(); }
				else openTop(a.i);
			} else if(it.enabled !== false) { closeAll(); activate(it); }
		} else if(a.kind == "row") {
			var lv = levels[a.l];
			if(lv === void) return;
			var row = lv.rows[a.r];
			if(row.sep || row.item.enabled === false) return;
			if(visibleChildren(row.item).count > 0) return;    // opens on hover
			var item = row.item;
			closeAll();
			activate(item);
		}
	}

	function activate(item)
	{
		if(item.radio) {
			var p = item.parent;
			if(p !== void) {
				var c = p.children;
				for(var i = 0; i < c.count; i++) if(c[i].radio && c[i].group == item.group) c[i].checked = (c[i] === item);
			}
		}
		Storages.fateDebugLog("menu: " + item.caption);
		try { item.onClick(); }
		catch(e) { Storages.fateDebugLog("menu item failed: " + item.caption + " : " + e.message); Debug.message("menu item failed: " + item.caption + " : " + e.message); }
	}

	function onKey(key)
	{
		if(key == VK_F10) { pinned = !pinned; if(!pinned && !inside) hover = false; layout(true); return true; }
		if(key == VK_ESCAPE && popupOpen) { closeAll(); return true; }
		return false;
	}
}

// ---------------------------------------------------------------------------
// FateVCursor: draws the mouse pointer when the engine has to emulate moving it
// (Wayland compositors do not let applications move the real pointer).
// ---------------------------------------------------------------------------
class FateVCursor
{
	var win, timer, lay, last = -2;
	function FateVCursor(w)
	{
		win = w;
		timer = new Timer(this, "onTick");
		timer.interval = 16;
		timer.enabled = true;
	}
	function finalize()
	{
		try { timer.enabled = false; invalidate timer; if(lay !== void) invalidate lay; } catch(e) {}
	}
	function build()
	{
		var pl = win.primaryLayer;
		if(pl === void || pl === null) return false;
		lay = new Layer(win, pl);
		lay.type = ltAlpha;
		lay.setImageSize(14, 21);
		lay.setSizeToImageSize();
		lay.fillRect(0, 0, 14, 21, 0);
		for(var y = 0; y < 18; y++) {
			var wo = (y < 13) ? y + 2 : 6;          // black outline of an arrow
			if(y == 0) wo = 2;
			lay.fillRect(0, y, wo, 1, 0xff000000);
			var wi = wo - 2;                        // white inside
			if(y >= 2 && wi > 0) lay.fillRect(1, y, wi, 1, 0xffffffff);
		}
		lay.enabled = false;
		lay.hitThreshold = 256;
		lay.absolute = 0x7fffffff;
		lay.visible = false;
		return true;
	}
	function onTick()
	{
		if(lay === void) { if(!build()) return; }
		var v = Storages.fateVirtualCursor();
		if(v < 0 || win.mouseCursorState != 0) {
			if(lay.visible) lay.visible = false;
			last = -2;
			return;
		}
		if(v != last) { lay.setPos(v & 0xffff, v >> 16); last = v; }
		if(!lay.visible) lay.visible = true;
	}
}

// called by Window.menu when a window's root menu is created
global.fateMenuBarAttach = function(win, root) {
	var bar = new FateMenuBar(win, root);
	win.__fateMenuBar = bar;
	if(Storages.fateVirtualCursor() != -2) win.__fateVCursor = new FateVCursor(win);
	win.__fateOrigKeyDown = win.onKeyDown;
	// F10 / Esc hook, chained in front of the window's own handler
	win.onKeyDown = function(key, shift, process) {
		if(this.__fateMenuBar !== void && this.__fateMenuBar.onKey(key)) return;
		if(this.__fateOrigKeyDown !== void) (this.__fateOrigKeyDown incontextof this)(key, shift, process);
	} incontextof win;
};
)TJS";

static const char FateMenuItemScript[] = R"TJS(
class MenuItem
{
	var window;
	var caption = "";
	var checked = false;
	var enabled = true;
	var visible = true;
	var group = 0;
	var radio = false;
	var shortcut = "";
	var bmp = void;
	var name = "";
	var parent = void;
	var children = [];

	function MenuItem(win = void, cap = "")
	{
		window = win;
		caption = cap;
	}
	function finalize() {}
	function onClick() {}
	function add(item)
	{
		children.add(item);
		item.parent = this;
	}
	function insert(item, idx)
	{
		children.insert(idx, item);
		item.parent = this;
	}
	function remove(item)
	{
		children.remove(item);
		item.parent = void;
	}
	function popup(flags = 0, x = 0, y = 0) { return 0; }
	function trackPopup(flags = 0, x = 0, y = 0) { return 0; }
	property root { getter() { var m = this; while(m.parent !== void) m = m.parent; return m; } }
	property index { getter() { return parent === void ? 0 : parent.children.find(this); } }
	property childrenCount { getter() { return children.count; } }
}
)TJS";
//---------------------------------------------------------------------------
extern void TVPFateAudioRegister();

#ifdef KRKRSDL2_FATE_WEBP
extern void TVPFateRegisterWebP();
#endif
void TVPFateCompatInstall()
{
#ifdef KRKRSDL2_FATE_WEBP
	// WebP images (HD patch assets)
	TVPFateRegisterWebP();
#endif
	// Ogg Vorbis / TCWF / MS-ADPCM decoders (formerly wuvorbis/wutcwf/wumsadp.dll)
	TVPFateAudioRegister();

	// KAGParser (formerly KAGParser.dll)
	try
	{
		iTJSDispatch2 *global = TVPGetScriptDispatch();
		if (global)
		{
			iTJSDispatch2 *kag = TVPCreateNativeClass_KAGParser();
			tTJSVariant val(kag);
			kag->Release();
			global->PropSet(TJS_MEMBERENSURE, TJS_W("KAGParser"), NULL, &val, global);

			// getDirList() (formerly dirlist.dll)
			iTJSDispatch2 *fn = TJSCreateNativeClassMethod(FateDirList);
			tTJSVariant fval(fn);
			fn->Release();
			global->PropSet(TJS_MEMBERENSURE, TJS_W("getDirList"), NULL, &fval, global);
			// Scripts.getObjectKeys (formerly scriptsEx.dll)
			tTJSVariant scripts;
			if (TJS_SUCCEEDED(global->PropGet(0, TJS_W("Scripts"), NULL, &scripts, global)) &&
				scripts.Type() == tvtObject)
			{
				iTJSDispatch2 *so = scripts.AsObjectNoAddRef();
				iTJSDispatch2 *gk = TJSCreateNativeClassMethod(FateGetObjectKeys);
				tTJSVariant gval(gk);
				gk->Release();
				so->PropSet(TJS_MEMBERENSURE, TJS_W("getObjectKeys"), NULL, &gval, so);
			}
			global->Release();
		}
	}
	catch (...)
	{
		TVPAddLog(TJS_W("fate_compat: failed to register KAGParser/getDirList"));
	}

	// system fonts
	try { FateLoadSystemFonts(); }
	catch (...) { TVPAddLog(TJS_W("fate_compat: font scan failed")); }

	// MenuItem (formerly menu.dll)
	try
	{
		tjs_string w;
		TVPUtf8ToUtf16(w, std::string(FateMenuItemScript));
		TVPExecuteExpression(ttstr(w));
	}
	catch (...)
	{
		TVPAddLog(TJS_W("fate_compat: failed to define MenuItem"));
	}

	// Debug.console / Debug.controller (the Windows debug windows) do not exist here;
	// give the game inert stand-ins so the Debug menu items do not throw
	try
	{
		tjs_string w;
		TVPUtf8ToUtf16(w, std::string(
			"try { if(typeof Debug.console == 'undefined') Debug.console = %[visible:false]; } catch(e) {}"
			"try { if(typeof Debug.controller == 'undefined') Debug.controller = %[visible:false]; } catch(e) {}"));
		TVPExecuteExpression(ttstr(w));
	}
	catch (...) {}

	// menu bar overlay
	try
	{
		tjs_string w;
		TVPUtf8ToUtf16(w, std::string(FateMenuBarScript));
		TVPExecuteExpression(ttstr(w));
	}
	catch (...)
	{
		TVPAddLog(TJS_W("fate_compat: failed to define the menu bar"));
	}
}

//---------------------------------------------------------------------------
// Plugins.link() shims
//
// With external plugins disabled Plugins.link() does nothing, but the game
// still calls the global functions its Windows plugins exported. When a
// script links one of them we define a replacement here. Signatures and
// expected behaviour are taken from docs/PLUGIN-INTERFACES.md, i.e. from how
// the game's scripts call them. Effects whose exact look cannot be inferred
// from the call sites (haze, lens, vortex, sparks, slide-open...) are inert:
// the game keeps running, the effect is simply absent.
//---------------------------------------------------------------------------

// util.dll: small Win32 helpers
static const char FateShimUtil[] = R"TJS(
function enumFont(win, fonts) { }                       // font list comes from System fonts
function DeleteFile(path)       { Storages.deleteFile(path); }
function MoveFile(src, dst)     { Storages.moveFile(src, dst); }
function CopyFile(src, dst)     { Storages.copyFile(src, dst, false); }
function CreateDirectory(path)  { Storages.createDirectory(path); }
function ExistDirectory(path)   { return Storages.isExistentDirectory(path) == true; }
function GetFileSize(path)      { return Storages.isExistentStorage(path) ? Storages.fstat(path).size : -1; }
function GetFileTime(path)      { return Storages.isExistentStorage(path) ? Storages.fstat(path).mtime : 0; }
function enabledIME(enable)     { }
function SetClipboardText(text) { }
function IsActiveWindow(win)    { return true; }
function SelectDirectory(caption, initialDir) { return ""; }   // caller falls back to the install dir
function GetDatetime()
{
	var d = new Date();
	return %[ year:d.getYear(), mon:d.getMonth()+1, day:d.getDate(),
	          hour:d.getHours(), minute:d.getMinutes(), second:d.getSeconds() ];
}
)TJS";

// drawer.dll: drawLine/drawAALine are native (fate_effects.cpp); the triangle is unused
static const char FateShimDrawer[] = R"TJS(
function drawAATriangle() { }
)TJS";

// filter.dll: Blur and Stretch are mapped onto core Layer operations;
// Noise is native (fate_effects.cpp)
static const char FateShimFilter[] = R"TJS(
function Blur(p) { p.layer.doBoxBlur(p.level, p.level); }
function Stretch(p)
{
	p.dest.stretchBlend(p.dleft, p.dtop, p.dwidth, p.dheight,
	                    p.src, p.sleft, p.stop, p.swidth, p.sheight, p.opacity);
}
function Smudge(p) { }   function Lens(p) { }     function InitLens() { }   function ReleaseLens() { }
function Contrast(p) { } function Vortex(p) { }
function initHaze(p) { } function doHaze(p) { }    function endHaze() { }
)TJS";

// resize.dll: area-average down-scaling, approximated with the core's linear stretch
static const char FateShimResize[] = R"TJS(
function AreaAverageReducation(src, dst)
{
	dst.operateStretch(0, 0, dst.imageWidth, dst.imageHeight,
	                   src, 0, 0, src.imageWidth, src.imageHeight, omOpaque, , stFastLinear);
}
)TJS";

static const char FateShimFireSpark[] = R"TJS(
function initFireSpark(layer, p) { }  function drawFireSpark(p) { }
function changeFireSpark(p) { }       function finishFireSpark() { }
)TJS";

static const char FateShimSlideOpen[] = R"TJS(
function initSlideOpen(o) { }  function drawSlideOpen() { }  function finishSlideOpen() { }
)TJS";

// wallpaper.dll sets the Windows desktop background; nothing to do here
static const char FateShimWallpaper[] = R"TJS(
function setwallpaper(p) { }
function getwallpaper() { return ""; }
)TJS";

// addFont.dll: System.addFont(storage) -> core Font.addFont
static const char FateShimAddFont[] = R"TJS(
System.addFont = function(storage, flag) {
	try { var r = Font.addFont(storage); Debug.message("addFont: " + storage + " ok"); Storages.fateDebugLog("addFont ok: " + storage); return r; }
	catch(e) { Debug.message("addFont failed: " + storage + " : " + e.message); Storages.fateDebugLog("addFont FAILED: " + storage + " : " + e.message); }
	return [];
};
)TJS";

// kri18n.dll: gettext-style __() / _x(); the game itself provides a .po based
// implementation (i18n.tjs), which we extend with a binary .mo reader
static const char FateShimI18n[] = R"TJS(
if(Storages.isExistentStorage("i18n.tjs")) {
	Scripts.execStorage("i18n.tjs");
	// the Windows build reads binary gettext .mo files; i18n.tjs only knows .po
	global.fateOrigLoadMo = global.loadMoFile;
	global.loadMoFile = function(moFile, domain) {
		if(domain === void) domain = global.defaultDomain;
		var d = Storages.fateLoadMo(moFile);
		if(d !== void) { global.domains[domain] = d; return; }
		global.fateOrigLoadMo(moFile, domain);
	};
}
else {
	global.removeAllMO = function() { };
	global.loadMoFile = function() { };
	global.removeMO = function() { };
	global.__ = function(text, domain) { return text; };
	global._x = function(text, context, domain) { return text; };
	global._n = function(single, plural, n, domain) { return n == 1 ? single : plural; };
}
)TJS";

static std::string FateBaseName(const ttstr &name)
{
	std::string n;
	TVPUtf16ToUtf8(n, name.AsStdString());
	size_t sl = n.find_last_of("/\\>");
	if (sl != std::string::npos) n = n.substr(sl + 1);
	size_t dot = n.find_last_of('.');
	if (dot != std::string::npos) n = n.substr(0, dot);
	for (size_t i = 0; i < n.size(); i++)
		if (n[i] >= 'A' && n[i] <= 'Z') n[i] = (char)(n[i] - 'A' + 'a');
	return n;
}

void TVPFateCompatOnPluginLink(const ttstr &name)
{
	static std::map<std::string, const char *> shims;
	static std::set<std::string> done;
	if (shims.empty())
	{
		shims["util"] = FateShimUtil;
		shims["drawer"] = FateShimDrawer;
		shims["filter"] = FateShimFilter;
		shims["resize"] = FateShimResize;
		shims["firespark"] = FateShimFireSpark;
		shims["slideopen"] = FateShimSlideOpen;
		shims["wallpaper"] = FateShimWallpaper;
		shims["kri18n"] = FateShimI18n;
		shims["addfont"] = FateShimAddFont;
	}
	std::string base = FateBaseName(name);
	std::map<std::string, const char *>::iterator it = shims.find(base);
	if (it == shims.end() || done.count(base)) return;
	done.insert(base);
	if (base == "drawer") TVPFateRegisterDrawer();
	if (base == "filter") TVPFateRegisterNoise();
	try
	{
		tjs_string w;
		TVPUtf8ToUtf16(w, std::string(it->second));
		TVPExecuteExpression(ttstr(w));
	}
	catch (...)
	{
		TVPAddLog(ttstr(TJS_W("fate_compat: shim failed for ")) + name);
	}
}
