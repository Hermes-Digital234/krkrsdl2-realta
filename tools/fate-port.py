#!/usr/bin/env python3
"""Turn a fresh krkrsdl2 checkout into the Fate/stay night Realta Nua port.

    python3 tools/fate-port.py /path/to/krkrsdl2          # apply
    python3 tools/fate-port.py /path/to/krkrsdl2 --check  # report only, change nothing

* copies overlay/ (new source files) into the tree,
* applies small text edits to upstream files. Edits are anchored on short code
  snippets (not line numbers), tolerate CRLF/whitespace differences, and are
  idempotent (a marker comment/identifier says "already applied"),
* exits non-zero and says exactly which anchor drifted if upstream changed
  too much, so a CI job can open an issue instead of shipping a broken build.
"""
import os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OVERLAY = os.path.join(HERE, "..", "overlay")

class Drift(Exception):
    pass

def read(p):
    with open(p, newline="") as f:
        t = f.read()
    nl = "\r\n" if "\r\n" in t else "\n"
    return t.replace("\r\n", "\n"), nl

def write(p, t, nl):
    with open(p, "w", newline="") as f:
        f.write(t.replace("\n", nl))

class Edit:
    """regex (whitespace in pattern matches any whitespace run) -> replacement"""
    def __init__(self, pattern, repl, count=1, marker=None):
        self.pattern, self.repl, self.count, self.marker = pattern, repl, count, marker
    def rx(self):
        parts = re.split(r"(\s+)", self.pattern.strip())
        return "".join(r"\s+" if i % 2 else re.escape(x) for i, x in enumerate(parts))

def apply_edits(root, rel, edits, check):
    p = os.path.join(root, rel.split('#')[0])
    if not os.path.exists(p):
        raise Drift(f"{rel}: file not found")
    text, nl = read(p)
    changed = False
    for e in edits:
        if e.marker and any(m in text for m in (e.marker if isinstance(e.marker, tuple) else (e.marker,))):
            continue
        rx = re.compile(e.rx())
        n = len(rx.findall(text))
        if n != e.count:
            raise Drift(f"{rel}: anchor matched {n}x (expected {e.count}): {e.pattern.strip()[:70]!r}")
        text = rx.sub(lambda m: e.repl, text)
        changed = True
    if changed and not check:
        write(p, text, nl)
    return changed

KICK = ("#define FATE_KICK() do { gFateFullClear = 3; if (this->bitmapCompletion && this->surface) { "
        "this->bitmapCompletion->update_rect = tTVPRect(0, 0, this->surface->w, this->surface->h); "
        "this->needsGraphicUpdate = true; } } while (0)\n")

EDITS = {
 "CMakeLists.txt": [
   Edit("list(APPEND KRKRSDL2_DEFINITIONS\n TVP_COMPILING_KRKRSDL2",
        "# Fate/stay night Realta Nua compatibility layer (fate-port)\n"
        "list(APPEND KRKRSDL2COMMON_SRC src/plugins/fate_compat.cpp src/plugins/fate_audio.cpp src/plugins/fate_effects.cpp src/plugins/fate_video.cpp src/plugins/fate_video_glue.cpp src/plugins/fate_stb_vorbis.c src/plugins/kagparser/KAGParser.cpp)\n"
        "list(APPEND KRKRSDL2COMMON_INCLUDES src/plugins src/plugins/kagparser)\n\n"
        "list(APPEND KRKRSDL2_DEFINITIONS\n\tTVP_COMPILING_KRKRSDL2", marker="fate_compat.cpp"),
 ],
 "meson.build": [
   Edit("'src/core/sdl2/SDLEntrypoint.cpp',\n]",
        "'src/core/sdl2/SDLEntrypoint.cpp',\n\t'src/plugins/fate_compat.cpp',\n\t'src/plugins/fate_audio.cpp',\n"
        "\t'src/plugins/fate_effects.cpp',\n\t'src/plugins/fate_video.cpp',\n\t'src/plugins/fate_video_glue.cpp',\n\t'src/plugins/fate_stb_vorbis.c',\n\t'src/plugins/kagparser/KAGParser.cpp',\n]\n"
        "krkrsdl2_includes += [\n\t'src/plugins',\n\t'src/plugins/kagparser',\n]", marker="fate_compat.cpp"),
 ],
 "src/core/base/sdl2/PluginImpl.cpp": [
   Edit("void TVPLoadPlugin(const ttstr & name)\n{",
        "extern void TVPFateCompatOnPluginLink(const ttstr &name);\nvoid TVPLoadPlugin(const ttstr & name)\n{\n\tTVPFateCompatOnPluginLink(name);",
        marker="TVPFateCompatOnPluginLink"),
 ],
 "src/core/base/sdl2/StorageImpl.cpp": [
   Edit("tTJSNativeClass * TVPCreateNativeClass_Storages()\n{\n tTJSNC_Storages *cls = new tTJSNC_Storages();",
        "extern void TVPFateCompatAddStorageMembers(tTJSNativeClass *cls);\ntTJSNativeClass * TVPCreateNativeClass_Storages()\n{\n"
        "\ttTJSNC_Storages *cls = new tTJSNC_Storages();\n\tTVPFateCompatAddStorageMembers(cls);", marker="TVPFateCompatAddStorageMembers"),
 ],
 "src/core/environ/sdl2/Application.cpp": [
   Edit("bool tTVPApplication::StartApplication( int argc, tjs_char* argv[] ) {",
        "extern void TVPFateCompatInstall();\nbool tTVPApplication::StartApplication( int argc, tjs_char* argv[] ) {", marker="TVPFateCompatInstall();\nbool"),
   Edit("TVPInitScriptEngine();\n engine_init = true;",
        "TVPInitScriptEngine();\n\t\tTVPFateCompatInstall();\n\t\tengine_init = true;", marker="TVPFateCompatInstall();\n\t\tengine_init"),
 ],
 "src/core/visual/sdl2/WindowImpl.cpp": [
   Edit("tTJSNativeClass * TVPCreateNativeClass_Window()\n{\n tTJSNativeClass *cls = new tTJSNC_Window();",
        "extern void TVPFateCompatAddWindowMembers(tTJSNativeClass *cls);\ntTJSNativeClass * TVPCreateNativeClass_Window()\n{\n"
        "\ttTJSNativeClass *cls = new tTJSNC_Window();\n\tTVPFateCompatAddWindowMembers(cls);", marker="TVPFateCompatAddWindowMembers"),
 ],
 # fullscreen / resize: clear the whole target (letterbox borders) and redraw in full
 "src/core/sdl2/SDLApplication.cpp": [
   Edit("void TVPWindowWindow::SetFullScreenMode(bool fullscreen)\n{",
        "static int gFateFullClear = 0;\n" + KICK + "void TVPWindowWindow::SetFullScreenMode(bool fullscreen)\n{", marker="gFateFullClear = 0"),
   Edit("SDL_SetWindowFullscreen(this->window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);",
        "SDL_SetWindowFullscreen(this->window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);\n\t\tFATE_KICK();", marker="0);\n\t\tFATE_KICK()"),
   Edit("rect.h = this->bitmapCompletion->update_rect.get_height();\n if (this->renderer)",
        "rect.h = this->bitmapCompletion->update_rect.get_height();\n"
        "\t\t\t\tbool fateForceFull = false;\n"
        "\t\t\t\tif (gFateFullClear > 0 && this->renderer && this->surface) { gFateFullClear--; rect.x = 0; rect.y = 0; rect.w = this->surface->w; rect.h = this->surface->h; fateForceFull = true; }\n"
        "\t\t\t\tif (this->renderer)", marker="fateForceFull = false"),
   Edit("if (logical_rect.w == rect.w && logical_rect.h == rect.h)",
        "if (fateForceFull || (logical_rect.w == rect.w && logical_rect.h == rect.h))", count=2, marker="fateForceFull ||"),
   Edit("case SDL_WINDOWEVENT_EXPOSED:\n {\n this->UpdateWindow(utNormal);",
        "case SDL_WINDOWEVENT_EXPOSED:\n\t\t\t\t\t\t{\n\t\t\t\t\t\t\tFATE_KICK();\n\t\t\t\t\t\t\tthis->UpdateWindow(utNormal);", marker="EXPOSED:\n\t\t\t\t\t\t{\n\t\t\t\t\t\t\tFATE_KICK"),
   Edit("#else\n this->UpdateWindow(utNormal);\n#endif\n TVPPostInputEvent(new tTVPOnResizeInputEvent",
        "#else\n\t\t\t\t\t\t\tFATE_KICK();\n\t\t\t\t\t\t\tthis->UpdateWindow(utNormal);\n#endif\n\t\t\t\t\t\t\tTVPPostInputEvent(new tTVPOnResizeInputEvent",
        marker=("#else\n\t\t\t\t\t\t\tFATE_KICK", "FateLogGeometry(\"window size event\"")),
 ],
 # fonts: a face list starting with "," (e.g. ",Sabon Next LT Pro Display") is taken unchecked;
 # if that font is not installed, use the next installed candidate or the default font
 "external/krkrz/visual/FreeTypeFontRasterizer.cpp": [
   Edit("tjs_string stdname = TVPFontSystem->GetBeingFont(face);\n faces.push_back( stdname );",
        "tjs_string stdname = TVPFontSystem->GetBeingFont(face);\n"
        "\t\t// A leading \",\" means \"take the first candidate without checking\". If that font\n"
        "\t\t// is not installed, use the next installed candidate, else the default font.\n"
        "\t\tif( !TVPFontSystem->FontExists( stdname ) ) {\n"
        "\t\t\tstd::vector<tjs_string> cands;\n"
        "\t\t\tsplit( face, tjs_string(TJS_W(\",\")), cands );\n"
        "\t\t\tstdname = TVPFontSystem->GetDefaultFontName();\n"
        "\t\t\tfor( auto& c : cands ) {\n"
        "\t\t\t\ttjs_string t = Trim( c );\n"
        "\t\t\t\tif( !t.empty() && TVPFontSystem->FontExists( t ) ) { stdname = t; break; }\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tfaces.push_back( stdname );", marker="take the first candidate without checking"),
 ],
 # last line of defence: a font that cannot be opened falls back to the default font
 "external/krkrz/visual/FreeType.cpp#2": [
   Edit("if( Faces[i]->Face == nullptr )\n {\n TVPThrowExceptionMessage(TVPFontCannotBeUsed, fontname[i] );\n }",
        "if( Faces[i]->Face == nullptr )\n\t\t{\n"
        "\t\t\t// requested font is not installed: use the default font instead of failing\n"
        "\t\t\tFaces[i]->Face.reset( FreeTypeFaceList->GetFace( tjs_string( TVPGetDefaultFontName() ), options & ~TVP_FACE_OPTIONS_FILE ) );\n"
        "\t\t}\n"
        "\t\tif( Faces[i]->Face == nullptr )\n\t\t{\n"
        "\t\t\tTVPThrowExceptionMessage(TVPFontCannotBeUsed, fontname[i] );\n\t\t}", marker="use the default font instead of failing"),
   Edit("tFreeTypeFace::tFreeTypeFace(const std::vector<tjs_string> &fontname, tjs_uint32 options)\n{",
        "extern const tjs_char *TVPGetDefaultFontName();\ntFreeTypeFace::tFreeTypeFace(const std::vector<tjs_string> &fontname, tjs_uint32 options)\n{",
        marker="extern const tjs_char *TVPGetDefaultFontName();\ntFreeTypeFace::tFreeTypeFace"),
 ],
 # trace font fallbacks to ./fate-debug.log
 "external/krkrz/visual/FreeType.cpp#4": [
   Edit("// requested font is not installed: use the default font instead of failing\n",
        "// requested font is not installed: use the default font instead of failing\n"
        "\t\t\t{ extern void TVPFateDebugLog(const char*); std::string m = \"font not found, using default: \"; for( auto c : fontname[i] ) m += ( c < 128 ? (char)c : '?' ); TVPFateDebugLog( m.c_str() ); }\n",
        marker="font not found, using default"),
 ],
 "external/krkrz/visual/FreeTypeFontRasterizer.cpp#2": [
   Edit("stdname = TVPFontSystem->GetDefaultFontName();\n",
        "{ extern void TVPFateDebugLog(const char*); std::string m = \"face list unavailable, using default: \"; for( auto c : face ) m += ( c < 128 ? (char)c : '?' ); TVPFateDebugLog( m.c_str() ); }\n\t\t\tstdname = TVPFontSystem->GetDefaultFontName();\n",
        marker="face list unavailable, using default"),
 ],
 # SDL's Wayland backend cannot move the pointer, which the game relies on (it moves the
 # cursor onto the selected button). Prefer X11 (XWayland) when available; an explicit
 # SDL_VIDEODRIVER environment variable still wins.
 "src/core/sdl2/SDLApplication.cpp#2": [
   Edit("if (!SDL_WasInit(SDL_INIT_VIDEO))\n\t{\n\t\tif (SDL_Init(SDL_INIT_VIDEO) < 0)",
        "if (!SDL_WasInit(SDL_INIT_VIDEO))\n\t{\n\t\tSDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, \"x11,wayland\", SDL_HINT_DEFAULT);\n\t\tif (SDL_Init(SDL_INIT_VIDEO) < 0)",
        marker="\"x11,wayland\""),
 ],
 # Wayland (and XWayland) cannot move the real pointer, but the game moves it onto the selected
 # button. Emulate it: remember a virtual pointer position, hide the system pointer while it is
 # in use, follow relative mouse motion, and let the game draw the pointer (FateVCursor).
 # the game zooms its window with setZoom() and then sizes it from zoomNumer/zoomDenom; without
 # a stored zoom every resize snaps back to 1:1. The renderer's logical size already scales the
 # picture to the window, so the zoom only has to be remembered.
 "src/core/sdl2/SDLApplication.cpp#4": [
   Edit("int lastMouseX;\n int lastMouseY;\n\n#ifdef KRKRSDL2_ENABLE_ZOOM\n tTVPRect FullScreenDestRect;",
        "int lastMouseX;\n\tint lastMouseY;\n\ttjs_int FateZoomNumer = 1, FateZoomDenom = 1;   // window zoom as set by the game\n\n#ifdef KRKRSDL2_ENABLE_ZOOM\n\ttTVPRect FullScreenDestRect;",
        marker="FateZoomNumer = 1"),
   Edit("void TVPWindowWindow::SetZoom(tjs_int numer, tjs_int denom, bool set_logical)\n{\n",
        "void TVPWindowWindow::SetZoom(tjs_int numer, tjs_int denom, bool set_logical)\n{\n"
        "\tif (set_logical && numer > 0 && denom > 0)\n\t{\n\t\ttjs_int a = numer, b = denom;\n\t\twhile (b) { tjs_int t = a % b; a = b; b = t; }\n"
        "\t\tthis->FateZoomNumer = numer / a;\n\t\tthis->FateZoomDenom = denom / a;\n\t}\n", marker="this->FateZoomNumer = numer / a"),
   Edit("return this->ZoomNumer;\n#else\n return 1;", "return this->ZoomNumer;\n#else\n\treturn this->FateZoomNumer;", marker="return this->FateZoomNumer"),
   Edit("return this->ZoomDenom;\n#else\n return 1;", "return this->ZoomDenom;\n#else\n\treturn this->FateZoomDenom;", marker="return this->FateZoomDenom"),
 ],
 "src/core/base/sdl2/SystemImpl.cpp": [
   Edit("return SDL_OpenURL(ntarget.c_str()) == 0;",
        "// a plain path (e.g. the readme) must be given as a file:// URL for xdg-open / the portal\n\t\t\tif (!ntarget.empty() && ntarget[0] == '/') ntarget = \"file://\" + ntarget;\n\t\t\treturn SDL_OpenURL(ntarget.c_str()) == 0;",
        marker="must be given as a file:// URL"),
 ],
 # The partial-update present assumes a two-buffer swap. GL drivers (Mesa llvmpipe, Wayland EGL) do
 # not guarantee that, so small updates such as the exit popup showed stale/garbled buffers. GL-type
 # renderers now redraw the whole frame on each present; the software renderer keeps partial updates.
 "src/core/sdl2/SDLApplication.cpp#5": [
   Edit("// By specification of SDL_RenderPresent",
        "static bool gFateFullPresent = false;   // GL-type renderers: redraw the whole frame on every present\n// By specification of SDL_RenderPresent",
        marker="static bool gFateFullPresent"),
   Edit("this->renderer = SDL_CreateRenderer(this->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);",
        "this->renderer = SDL_CreateRenderer(this->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);\n"
        "\t\tif (this->renderer)\n\t\t{\n\t\t\tSDL_RendererInfo fateRI;\n\t\t\tif (SDL_GetRendererInfo(this->renderer, &fateRI) == 0)\n\t\t\t{\n"
        "\t\t\t\tgFateFullPresent = strcmp(fateRI.name, \"software\") != 0;\n"
        "\t\t\t\textern void TVPFateDebugLog(const char*);\n\t\t\t\tstd::string fm = std::string(\"renderer: \") + fateRI.name;\n\t\t\t\tTVPFateDebugLog(fm.c_str());\n\t\t\t}\n\t\t}",
        marker="gFateFullPresent = strcmp"),
   Edit("if (fateForceFull || (logical_rect.w == rect.w && logical_rect.h == rect.h))\n{\n// Clear extra artifacts\nSDL_RenderSetLogicalSize",
        "if (fateForceFull || gFateFullPresent || (logical_rect.w == rect.w && logical_rect.h == rect.h))\n\t\t\t\t{\n\t\t\t\t\t// Clear extra artifacts\n\t\t\t\t\tSDL_RenderSetLogicalSize",
        marker="fateForceFull || gFateFullPresent"),
   Edit("SDL_RenderCopy(this->renderer, this->texture, &rect, &rect);\n#endif\n}\nSDL_RenderPresent",
        "if (gFateFullPresent) SDL_RenderCopy(this->renderer, this->texture, nullptr, nullptr);\n\t\t\t\t\telse SDL_RenderCopy(this->renderer, this->texture, &rect, &rect);\n#endif\n\t\t\t\t}\n\t\t\t\tSDL_RenderPresent",
        marker=("if (gFateFullPresent) SDL_RenderCopy", "SDL_Rect fsrc")),
   Edit("if (fateForceFull || (logical_rect.w == rect.w && logical_rect.h == rect.h))\n{\n// Clear extra artifacts (for the back buffer)",
        "if (!gFateFullPresent && (fateForceFull || (logical_rect.w == rect.w && logical_rect.h == rect.h)))\n\t\t\t\t{\n\t\t\t\t\t// Clear extra artifacts (for the back buffer)",
        marker="if (!gFateFullPresent && (fateForceFull"),
   Edit("if (this->texture)\n{\nSDL_RenderCopy(this->renderer, this->texture, &rect, &rect);\n}\n#endif\nthis->hasDrawn = true;",
        "if (this->texture && !gFateFullPresent)\n\t\t\t\t{\n\t\t\t\t\tSDL_RenderCopy(this->renderer, this->texture, &rect, &rect);\n\t\t\t\t}\n#endif\n\t\t\t\tthis->hasDrawn = true;",
        marker="this->texture && !gFateFullPresent"),
 ],
 "src/core/sdl2/SDLApplication.cpp#3": [
   Edit("static SDL_GameController **sdl_controllers = nullptr;",
        "// --- virtual pointer (compositors that refuse to warp the pointer) ---\n"
        "struct FateVCState { bool active; int x, y, px, py; bool havePrev; };\n"
        "static FateVCState gFateVC = { false, 0, 0, 0, 0, false };\n"
        "static bool FateVCEnabled() {\n"
        "\tstatic int v = -1;\n"
        "\tif (v < 0) v = getenv(\"FATE_VCURSOR\") ? 1 : 0;   // experimental, off by default\n"
        "\treturn v == 1;\n}\n"
        "static SDL_Cursor *FateBlankCursor() {\n"
        "\tstatic SDL_Cursor *c = nullptr;\n"
        "\tif (!c) { Uint8 d[8] = {0}, m[8] = {0}; c = SDL_CreateCursor(d, m, 8, 8, 0, 0); }\n"
        "\treturn c;\n}\n"
        "bool TVPFateGetVirtualCursor(int *x, int *y) {\n"
        "\tif (!gFateVC.active) return false;\n"
        "\t*x = gFateVC.x; *y = gFateVC.y; return true;\n}\n"
        "static SDL_GameController **sdl_controllers = nullptr;", marker="gFateVC = {"),
   Edit("void TVPWindowWindow::GetCursorPos(tjs_int &x, tjs_int &y)\n{\n\tx = this->lastMouseX;\n\ty = this->lastMouseY;\n",
        "void TVPWindowWindow::GetCursorPos(tjs_int &x, tjs_int &y)\n{\n\tx = this->lastMouseX;\n\ty = this->lastMouseY;\n"
        "\tif (gFateVC.active) return;\n", marker="if (gFateVC.active) return;"),
   Edit("void TVPWindowWindow::SetCursorPos(tjs_int x, tjs_int y)\n{\n\tthis->RestoreMouseCursor();\n\tif (!this->window)\n\t{\n\t\treturn;\n\t}\n",
        "void TVPWindowWindow::SetCursorPos(tjs_int x, tjs_int y)\n{\n\tthis->RestoreMouseCursor();\n\tif (!this->window)\n\t{\n\t\treturn;\n\t}\n"
        "\tif (FateVCEnabled())\n\t{\n\t\tgFateVC.active = true;\n\t\tgFateVC.x = x;\n\t\tgFateVC.y = y;\n"
        "\t\tthis->lastMouseX = x;\n\t\tthis->lastMouseY = y;\n\t\tSDL_SetCursor(FateBlankCursor());\n\t\treturn;\n\t}\n"
        "\t// Wayland compositors ignore pointer warps; do nothing rather than fake a position\n"
        "\tif (getenv(\"WAYLAND_DISPLAY\") && !getenv(\"FATE_REAL_WARP\"))\n\t{\n\t\treturn;\n\t}\n",
        marker="gFateVC.active = true;"),
   Edit("this->RestoreMouseCursor();\n this->lastMouseX = event.motion.x;\n this->lastMouseY = event.motion.y;\n this->TranslateWindowToDrawArea(this->lastMouseX, this->lastMouseY);",
        "this->RestoreMouseCursor();\n"
        "\t\t\t\t\t{\n\t\t\t\t\t\tint rx = event.motion.x, ry = event.motion.y;\n"
        "\t\t\t\t\t\tthis->TranslateWindowToDrawArea(rx, ry);\n"
        "\t\t\t\t\t\tif (gFateVC.active)\n\t\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\tif (gFateVC.havePrev) { gFateVC.x += rx - gFateVC.px; gFateVC.y += ry - gFateVC.py; }\n"
        "\t\t\t\t\t\t\ttjs_int iw = this->GetInnerWidth(), ih = this->GetInnerHeight();\n"
        "\t\t\t\t\t\t\tif (gFateVC.x < 0) gFateVC.x = 0; if (gFateVC.y < 0) gFateVC.y = 0;\n"
        "\t\t\t\t\t\t\tif (iw > 0 && gFateVC.x >= iw) gFateVC.x = iw - 1; if (ih > 0 && gFateVC.y >= ih) gFateVC.y = ih - 1;\n"
        "\t\t\t\t\t\t\tthis->lastMouseX = gFateVC.x; this->lastMouseY = gFateVC.y;\n"
        "\t\t\t\t\t\t\tSDL_SetCursor(FateBlankCursor());\n"
        "\t\t\t\t\t\t}\n\t\t\t\t\t\telse { this->lastMouseX = rx; this->lastMouseY = ry; }\n"
        "\t\t\t\t\t\tgFateVC.px = rx; gFateVC.py = ry; gFateVC.havePrev = true;\n\t\t\t\t\t}",
        marker="gFateVC.px = rx;"),
   Edit("this->lastMouseX = event.button.x;\n this->lastMouseY = event.button.y;\n this->TranslateWindowToDrawArea(this->lastMouseX, this->lastMouseY);",
        "if (!gFateVC.active)\n\t\t\t\t\t\t{\n\t\t\t\t\t\tthis->lastMouseX = event.button.x;\n\t\t\t\t\t\tthis->lastMouseY = event.button.y;\n"
        "\t\t\t\t\t\tthis->TranslateWindowToDrawArea(this->lastMouseX, this->lastMouseY);\n\t\t\t\t\t\t}",
        marker="if (!gFateVC.active)\n"),
   Edit("case SDL_WINDOWEVENT_LEAVE:\n {\n",
        "case SDL_WINDOWEVENT_LEAVE:\n\t\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\tif (gFateVC.active) { gFateVC.active = false; gFateVC.havePrev = false; this->SetDefaultMouseCursor(); }\n",
        marker="gFateVC.active = false;"),
 ],
 # WebP images (the HD patch ships .webp rule/graphic replacements): optional, needs libwebp
 "CMakeLists.txt#2": [
   Edit("list(APPEND KRKRSDL2_DEFINITIONS\n TVP_COMPILING_KRKRSDL2",
        "# WebP support through the system libwebp (optional)\n"
        "find_package(PkgConfig QUIET)\n"
        "if(PkgConfig_FOUND)\n\tpkg_check_modules(FATE_WEBP QUIET IMPORTED_TARGET libwebp)\nendif()\n"
        "if(FATE_WEBP_FOUND)\n"
        "\tmessage(STATUS \"fate-port: libwebp found, WebP images enabled\")\n"
        "\tlist(APPEND KRKRSDL2COMMON_SRC src/plugins/fate_webp.cpp)\n"
        "\tlist(APPEND KRKRSDL2_DEFINITIONS KRKRSDL2_FATE_WEBP)\n"
        "\tlist(APPEND KRKRSDL2_LIBS PkgConfig::FATE_WEBP)\n"
        "else()\n"
        "\tmessage(WARNING \"fate-port: libwebp not found - .webp images (HD transition rules etc.) will not load. Install libwebp (Arch: pacman -S libwebp).\")\n"
        "endif()\n\n"
        "list(APPEND KRKRSDL2_DEFINITIONS\n\tTVP_COMPILING_KRKRSDL2", marker="fate-port: libwebp found"),
 ],
 # compile-time switches: OPTION_FATE_DEBUG_LOG (default ON) and OPTION_FATE_VIRTUAL_CURSOR (default OFF)
 "CMakeLists.txt#3": [
   Edit("list(APPEND KRKRSDL2_DEFINITIONS\n TVP_COMPILING_KRKRSDL2",
        "# fate-port build options\n"
        "option(OPTION_FATE_DEBUG_LOG \"Write diagnostics (menu clicks, font fallbacks, renderer) to ./fate-debug.log\" ON)\n"
        "option(OPTION_FATE_VIRTUAL_CURSOR \"Experimental: emulate moving the mouse pointer by drawing it (Wayland cannot warp it)\" OFF)\n"
        "if(OPTION_FATE_DEBUG_LOG)\n\tlist(APPEND KRKRSDL2_DEFINITIONS KRKRSDL2_FATE_DEBUG_LOG)\nendif()\n"
        "if(OPTION_FATE_VIRTUAL_CURSOR)\n\tlist(APPEND KRKRSDL2_DEFINITIONS KRKRSDL2_FATE_VCURSOR)\nendif()\n\n"
        "list(APPEND KRKRSDL2_DEFINITIONS\n\tTVP_COMPILING_KRKRSDL2", marker="OPTION_FATE_DEBUG_LOG"),
 ],
 "src/core/sdl2/SDLApplication.cpp#6": [
   Edit("if (v < 0) v = getenv(\"FATE_VCURSOR\") ? 1 : 0; // experimental, off by default\n return v == 1;",
        "#ifdef KRKRSDL2_FATE_VCURSOR\n\tv = 1;   // compiled in (-DOPTION_FATE_VIRTUAL_CURSOR=ON)\n#else\n\tv = 0;\n#endif\n\treturn v == 1;",
        marker="compiled in (-DOPTION_FATE_VIRTUAL_CURSOR=ON)"),
 ],
 # the executable is called fate-sdl
 "CMakeLists.txt#4": [
   Edit("OUTPUT_NAME \"krkrsdl2\")", "OUTPUT_NAME \"fate-sdl\")", marker="OUTPUT_NAME \"fate-sdl\""),
 ],

 # The renderer's scaling/viewport can go stale after a fullscreen toggle or resize, and not every
 # compositor sends the events the engine listened for (the port used to redraw only on events).
 # Detect an output-size change at present time, re-apply the logical size, redraw everything,
 # and trace the geometry to fate-debug.log (OPTION_FATE_DEBUG_LOG).
 "src/core/sdl2/SDLApplication.cpp#7": [
   Edit("#include \"tjsCommHead.h\"", "#include \"tjsCommHead.h\"\n#include <map>\n#include <utility>", marker="#include <utility>"),
   Edit("static bool gFateFullPresent = false;   // GL-type renderers: redraw the whole frame on every present",
        "static bool gFateFullPresent = false;   // GL-type renderers: redraw the whole frame on every present\n"
        "// After the window size changed (fullscreen toggle, resize) SDL's renderer can keep the old\n"
        "// viewport/clip; re-applying the logical size makes it recompute scale, viewport and mouse mapping.\n"
        "static void FateRefreshLogical(SDL_Renderer *r)\n{\n\tif (!r) return;\n\tint lw = 0, lh = 0;\n\tSDL_RenderGetLogicalSize(r, &lw, &lh);\n\tif (lw <= 0 || lh <= 0) return;\n"
        "\tSDL_RenderSetLogicalSize(r, 0, 0);\n\tSDL_RenderSetViewport(r, nullptr);\n\tSDL_RenderSetClipRect(r, nullptr);\n\tSDL_RenderSetLogicalSize(r, lw, lh);\n}\n"
        "static int gFateMotionLog = 0;   // how many mouse-motion events are still traced\n"
        "static std::map<SDL_Renderer *, std::pair<int, int> > gFateLastOutput;   // output size seen at the last present\n"
        "static void FateLogGeometry(const char *what, SDL_Window *win, SDL_Renderer *r)\n{\n\textern void TVPFateDebugLog(const char*);\n"
        "\tint ow = 0, oh = 0, ww = 0, wh = 0, lw = 0, lh = 0;\n\tSDL_Rect vp = {0, 0, 0, 0};\n\tfloat sx = 0, sy = 0;\n"
        "\tif (r) { SDL_GetRendererOutputSize(r, &ow, &oh); SDL_RenderGetViewport(r, &vp); SDL_RenderGetScale(r, &sx, &sy); SDL_RenderGetLogicalSize(r, &lw, &lh); }\n"
        "\tif (win) SDL_GetWindowSize(win, &ww, &wh);\n\tchar b[320];\n"
        "\tsnprintf(b, sizeof(b), \"%s: window %dx%d output %dx%d logical %dx%d viewport %d,%d %dx%d scale %.3f flags 0x%x\",\n"
        "\t\twhat, ww, wh, ow, oh, lw, lh, vp.x, vp.y, vp.w, vp.h, sx, win ? (unsigned)SDL_GetWindowFlags(win) : 0u);\n\tTVPFateDebugLog(b);\n}",
        marker="FateRefreshLogical(SDL_Renderer"),
   Edit("#else\n FATE_KICK();\n this->UpdateWindow(utNormal);\n#endif\n TVPPostInputEvent(new tTVPOnResizeInputEvent",
        "#else\n\t\t\t\t\t\t\tFateRefreshLogical(this->renderer);\n\t\t\t\t\t\t\tFateLogGeometry(\"window size event\", this->window, this->renderer);\n\t\t\t\t\t\t\tgFateMotionLog = 40;\n"
        "\t\t\t\t\t\t\tFATE_KICK();\n\t\t\t\t\t\t\tthis->UpdateWindow(utNormal);\n#endif\n\t\t\t\t\t\t\tTVPPostInputEvent(new tTVPOnResizeInputEvent",
        marker="FateLogGeometry(\"window size event\""),
   Edit("int rx = event.motion.x, ry = event.motion.y;",
        "int rx = event.motion.x, ry = event.motion.y;\n"
        "\t\t\t\t\t\tif (gFateMotionLog > 0) { gFateMotionLog--; extern void TVPFateDebugLog(const char*); char mb[120]; snprintf(mb, sizeof(mb), \"mouse motion: sdl %d,%d\", rx, ry); TVPFateDebugLog(mb); }",
        marker="mouse motion: sdl"),
   Edit("bool fateForceFull = false;",
        "bool fateForceFull = false;\n"
        "\t\t\t\tif (this->renderer && this->window)\n\t\t\t\t{\n"
        "\t\t\t\t\t// the output size changed without us hearing about it (compositors differ in the events they send):\n"
        "\t\t\t\t\t// make SDL recompute its scaling and redraw everything, including the borders\n"
        "\t\t\t\t\tint fow = 0, foh = 0;\n\t\t\t\t\tSDL_GetRendererOutputSize(this->renderer, &fow, &foh);\n"
        "\t\t\t\t\tstd::pair<int, int> &fl = gFateLastOutput[this->renderer];\n\t\t\t\t\tif (fl.first != fow || fl.second != foh)\n\t\t\t\t\t{\n"
        "\t\t\t\t\t\tfl.first = fow; fl.second = foh;\n\t\t\t\t\t\tFateRefreshLogical(this->renderer);\n"
        "\t\t\t\t\t\tFateLogGeometry(\"output size changed\", this->window, this->renderer);\n\t\t\t\t\t\tgFateMotionLog = 40;\n\t\t\t\t\t\tgFateFullClear = 3;\n\t\t\t\t\t}\n\t\t\t\t}",
        marker="output size changed"),
 ],
 # GL-type renderers: clear the whole target with SDL_RenderClear (it ignores viewport and clip) before
 # every frame; the logical-size/fill trick left stale windowed-mode pixels in the fullscreen borders.
 "src/core/sdl2/SDLApplication.cpp#8": [
   Edit("if (fateForceFull || gFateFullPresent || (logical_rect.w == rect.w && logical_rect.h == rect.h))\n{\n// Clear extra artifacts\nSDL_RenderSetLogicalSize",
        "// SDL_RenderClear ignores viewport/clip, so the letterbox borders are certainly cleared\n\t\t\t\tif (gFateFullPresent) SDL_RenderClear(this->renderer);\n"
        "\t\t\t\tif (fateForceFull || gFateFullPresent || (logical_rect.w == rect.w && logical_rect.h == rect.h))\n\t\t\t\t{\n\t\t\t\t\t// Clear extra artifacts\n\t\t\t\t\tSDL_RenderSetLogicalSize",
        marker="if (gFateFullPresent) SDL_RenderClear"),
 ],
 # a .webp next to a same-named .png/.tlg wins when no extension is given (HD replacements)
 "external/krkrz/visual/GraphicsLoaderIntf.cpp": [
   Edit("handler = & i.GetValue();\n }\n else\n {\n handler = TVPGraphicType.Hash.Find(ext);",
        "handler = & i.GetValue();\n"
        "\t\t{\n\t\t\t// the HD patch ships .webp replacements of original images: prefer them\n"
        "\t\t\ttTVPGraphicHandlerType *wh = TVPGraphicType.Hash.Find(TJS_W(\".webp\"));\n"
        "\t\t\tttstr wn = _name + TJS_W(\".webp\");\n"
        "\t\t\tif(wh && name != wn && TVPIsExistentStorage(wn)) { name = wn; handler = wh; }\n"
        "\t\t}\n\t}\n\telse\n\t{\n\t\thandler = TVPGraphicType.Hash.Find(ext);", marker="prefer them"),
 ],
 # font names: a font must be found under every name Windows would know it by
 # (legacy family, typographic family, full name), not only FreeType's family_name
 "external/krkrz/visual/FreeType.cpp#5": [
   Edit("name_tmp.name_id == TT_NAME_ID_FULL_NAME) {",
        "( name_tmp.name_id == TT_NAME_ID_FULL_NAME || name_tmp.name_id == TT_NAME_ID_FONT_FAMILY || name_tmp.name_id == 16 /*typographic family*/ )) {",
        marker="16 /*typographic family*/"),
   Edit("int numcharmap = face->num_charmaps;",
        "if( faces ) {\n\t\t\t\tfor( auto& a : info->aliases ) {\n"
        "\t\t\t\t\tif( std::find( faces->begin(), faces->end(), a ) == faces->end() ) faces->push_back( a );\n"
        "\t\t\t\t}\n\t\t\t}\n\t\t\tint numcharmap = face->num_charmaps;",
        marker="for( auto& a : info->aliases )"),
   Edit("if( faces ) faces->push_back( (*i)->facename );\n else return false;",
        "if( faces ) {\n\t\t\t\tfaces->push_back( (*i)->facename );\n"
        "\t\t\t\tfor( auto& a : (*i)->aliases ) faces->push_back( a );\n\t\t\t}\n\t\t\telse return false;",
        marker="for( auto& a : (*i)->aliases )"),
 ],
 # text baseline: use the OS/2 "win" ascent (what GDI-based Windows builds use) instead of hhea
 "external/krkrz/visual/FreeType.cpp#3": [
   Edit("int baseline = (int)( face->ascender ) * face->size->metrics.y_ppem / face->units_per_EM;",
        "int baseline = TVPFateAscent( face );", marker="TVPFateAscent( face )"),
   Edit("extern const tjs_char *TVPGetDefaultFontName();\ntFreeTypeFace::tFreeTypeFace(",
        "#include FT_TRUETYPE_TABLES_H\n"
        "tjs_int TVPFateAscent( FT_Face face ) {\n"
        "\tTT_OS2 *os2 = (TT_OS2*)FT_Get_Sfnt_Table( face, FT_SFNT_OS2 );\n"
        "\tFT_Short asc = ( os2 && os2->usWinAscent ) ? (FT_Short)os2->usWinAscent : face->ascender;\n"
        "\treturn (tjs_int)asc * face->size->metrics.y_ppem / face->units_per_EM;\n}\n"
        "extern const tjs_char *TVPGetDefaultFontName();\ntFreeTypeFace::tFreeTypeFace(",
        marker="tjs_int TVPFateAscent( FT_Face face )"),
 ],
 "external/krkrz/visual/FreeType.h": [
   Edit("return face->ascender * ppem / upe;",
        "extern tjs_int TVPFateAscent( FT_Face face ); (void)ppem; (void)upe; return TVPFateAscent( face );", marker="TVPFateAscent( face )"),
 ],
 # system fonts: fall back from the extra-font list to the system fonts, and to another style
 "external/krkrz/visual/FreeType.cpp": [
   Edit("} else {\n FaceKey key( facename, options&( TVP_TF_ITALIC | TVP_TF_BOLD ) );\n auto i = systemfont_.find( key );",
        "}\n\t{\n\t\t// not among the extra (addFont) faces: also look at the system fonts\n"
        "\t\tFaceKey key( facename, options&( TVP_TF_ITALIC | TVP_TF_BOLD ) );\n\t\tauto i = systemfont_.find( key );\n"
        "\t\tif( i == systemfont_.end() ) {\n"
        "\t\t\t// requested style is not installed: use another style of the same face\n"
        "\t\t\tstatic const tjs_uint style_order[] = { 0, TVP_TF_BOLD, TVP_TF_ITALIC, TVP_TF_BOLD|TVP_TF_ITALIC };\n"
        "\t\t\tfor( size_t k = 0; k < sizeof(style_order)/sizeof(style_order[0]); k++ ) {\n"
        "\t\t\t\ti = systemfont_.find( FaceKey( facename, style_order[k] ) );\n"
        "\t\t\t\tif( i != systemfont_.end() ) break;\n\t\t\t}\n\t\t}", marker="also look at the system fonts"),
 ],
}

# --- movie playback (ffmpeg based; the engine's own backends are Windows-only) ---------------------------------------
def video_edits():
    F = "VideoOvlImpl.cpp"
    def head(sig, body, tag):
        return Edit(sig + "\n{", sig + "\n{\n#ifdef FATE_VIDEO\n" + body + "\t/* " + tag + " */\n#endif", marker="/* " + tag + " */")
    def getter(sig, what, cast):
        return head(sig, "\t{ double fv; if (FateQuery(%d, fv)) return (%s)fv; }\n" % (what, cast), "fate:" + sig)
    def setter(sig, what, arg):
        return head(sig, "\tif (FateSet(%d, (double)%s)) return;\n" % (what, arg), "fate:" + sig)
    V = "tTJSNI_VideoOverlay::"
    e = [
      head("void " + V + "Open(const ttstr &_name)", "\tFateOpen(_name);\n\treturn;\n", "fate:Open"),
      head("void " + V + "Close()", "\tFateClose(true);\n\treturn;\n", "fate:Close"),
      head("void " + V + "Shutdown()", "\tFateClose(false);\n\treturn;\n", "fate:Shutdown"),
      head("void " + V + "Play()", "\tFatePlay();\n\treturn;\n", "fate:Play"),
      head("void " + V + "Stop()", "\tFateStop();\n\treturn;\n", "fate:Stop"),
      head("void " + V + "Pause()", "\tFatePause();\n\treturn;\n", "fate:Pause"),
      head("void " + V + "Rewind()", "\tFateRewind();\n\treturn;\n", "fate:Rewind"),
      head("void " + V + "Prepare()", "\tFatePrepare();\n\treturn;\n", "fate:Prepare"),
      head("void " + V + "SetRectangleToVideoOverlay()", "\tFateApplyOverlay();\n\treturn;\n", "fate:SetRect"),
      head("void " + V + "ResetOverlayParams()", "\tFateApplyOverlay();\n\treturn;\n", "fate:ResetOverlay"),
      Edit("void " + V + "SetVisible(bool b)\n{\n\tVisible = b;",
           "void " + V + "SetVisible(bool b)\n{\n\tVisible = b;\n#ifdef FATE_VIDEO\n\tFateApplyOverlay();\n\treturn; /* fate:SetVisible */\n#endif",
           marker="/* fate:SetVisible */"),
      setter("void " + V + "SetTimePosition( tjs_uint64 p )", 5, "p"),
      getter("tjs_uint64 " + V + "GetTimePosition()", 5, "tjs_uint64"),
      setter("void " + V + "SetFrame( tjs_int f )", 6, "f"),
      getter("tjs_int " + V + "GetFrame()", 6, "tjs_int"),
      getter("tjs_real " + V + "GetFPS()", 2, "tjs_real"),
      getter("tjs_int " + V + "GetNumberOfFrame()", 3, "tjs_int"),
      getter("tjs_int64 " + V + "GetTotalTime()", 4, "tjs_int64"),
      getter("tjs_real " + V + "GetPlayRate()", 11, "tjs_real"),
      setter("void " + V + "SetPlayRate(tjs_real r)", 11, "r"),
      getter("tjs_int " + V + "GetAudioBalance()", 10, "tjs_int"),
      setter("void " + V + "SetAudioBalance(tjs_int b)", 10, "b"),
      getter("tjs_int " + V + "GetAudioVolume()", 9, "tjs_int"),
      setter("void " + V + "SetAudioVolume(tjs_int b)", 9, "b"),
      getter("tjs_uint " + V + "GetNumberOfAudioStream()", 7, "tjs_uint"),
      getter("tjs_int " + V + "GetEnabledAudioStream()", 8, "tjs_int"),
      getter("tjs_uint " + V + "GetNumberOfVideoStream()", 12, "tjs_uint"),
      getter("tjs_int " + V + "GetOriginalWidth()", 0, "tjs_int"),
      getter("tjs_int " + V + "GetOriginalHeight()", 1, "tjs_int"),
    ]
    return {
      # trees patched by an earlier version of this script
      "CMakeLists.txt#5": [
        Edit("src/plugins/fate_effects.cpp src/plugins/fate_stb_vorbis.c",
             "src/plugins/fate_effects.cpp src/plugins/fate_video.cpp src/plugins/fate_video_glue.cpp src/plugins/fate_stb_vorbis.c",
             marker="fate_video.cpp"),
      ],
      "meson.build#2": [
        Edit("'src/plugins/fate_effects.cpp',\n\t'src/plugins/fate_stb_vorbis.c',",
             "'src/plugins/fate_effects.cpp',\n\t'src/plugins/fate_video.cpp',\n\t'src/plugins/fate_video_glue.cpp',\n\t'src/plugins/fate_stb_vorbis.c',",
             marker="fate_video.cpp"),
      ],
      "src/core/visual/sdl2/" + F: e + [
        # the pointer is never initialised outside the Windows build, so "is a movie open?" read garbage
        Edit("void " + V + "SetMode( tTVPVideoOverlayMode m )\n{", 
             "void " + V + "SetMode( tTVPVideoOverlayMode m )\n{\n#ifdef FATE_VIDEO\n\tif( !FateState ) { Mode = m; return; } /* fate:SetMode */\n#endif",
             marker="/* fate:SetMode */"),
      ],
      "src/core/visual/sdl2/VideoOvlImpl.h": [
        Edit("class iTVPVideoOverlay;\nclass tTJSNI_VideoOverlay : public tTJSNI_BaseVideoOverlay\n{",
             "// movie playback through ffmpeg on platforms without the Windows backends (src/plugins/fate_video_glue.cpp)\n"
             "#if !(defined(_WIN32) && defined(KRKRSDL2_USE_WIN32_EVENT_QUEUE) && defined(KRKRSDL2_ENABLE_VIDEOOVERLAY))\n"
             "#define FATE_VIDEO 1\n#endif\n"
             "struct FateVOState;\nclass iTVPVideoOverlay;\nclass tTJSNI_VideoOverlay : public tTJSNI_BaseVideoOverlay\n{",
             marker="#define FATE_VIDEO 1"),
        Edit("\tiTVPVideoOverlay *VideoOverlay;", "\tiTVPVideoOverlay *VideoOverlay = nullptr;", marker="*VideoOverlay = nullptr"),
        Edit("public:\n\ttTJSNI_VideoOverlay();",
             "public:\n\tFateVOState *FateState = nullptr;\n\tbool FateOpen(const ttstr &name);\n\tvoid FateClose(bool fire);\n"
             "\tvoid FatePlay();\n\tvoid FateStop();\n\tvoid FatePause();\n\tvoid FateRewind();\n\tvoid FatePrepare();\n"
             "\tvoid FateApplyOverlay();\n\tvoid FateTick();\n\tbool FateQuery(int what, double &out);\n\tbool FateSet(int what, double v);\n\n"
             "\ttTJSNI_VideoOverlay();", marker="FateVOState *FateState"),
      ],
      # window size: remember what was asked for until the window manager answers. SDL_GetWindowSize lags behind
      # SDL_SetWindowSize, so the game setting innerWidth and then innerHeight (as the HD wrapper does when the aspect
      # ratio changes) made the second call put the old width back.
      "src/core/sdl2/SDLApplication.cpp#10": [
        Edit("static std::map<SDL_Renderer *, std::pair<int, int> > gFateLastOutput;",
             "static std::map<SDL_Renderer *, std::pair<int, int> > gFateLastOutput;\n"
             "static std::map<SDL_Window *, std::pair<std::pair<int, int>, Uint32> > gFateReqSize;   // requested window size and when, until a size event arrives (or 400 ms pass)\n"
             "static void FateGetWinSize(SDL_Window *w, int *pw, int *ph)\n{\n\tint a = 0, b = 0;\n\tSDL_GetWindowSize(w, &a, &b);\n"
             "\tstd::map<SDL_Window *, std::pair<std::pair<int, int>, Uint32> >::iterator it = gFateReqSize.find(w);\n"
             "\tif (it != gFateReqSize.end() && SDL_GetTicks() - it->second.second < 400) { a = it->second.first.first; b = it->second.first.second; }\n\tif (pw) *pw = a;\n\tif (ph) *ph = b;\n}\n"
             "static void FateSetWinSize(SDL_Window *w, int ww, int wh)\n{\n\tint a = 0, b = 0;\n\tSDL_GetWindowSize(w, &a, &b);\n"
             "\tgFateReqSize[w] = std::make_pair(std::make_pair(ww, wh), SDL_GetTicks());\n\tSDL_SetWindowSize(w, ww, wh);\n"
             "\tif (a != ww || b != wh) { char m[96]; snprintf(m, sizeof(m), \"set window size %dx%d (was %dx%d)\", ww, wh, a, b); extern void TVPFateDebugLog(const char*); TVPFateDebugLog(m); }\n}",
             marker="FateSetWinSize(SDL_Window"),
        Edit("int h;\n SDL_GetWindowSize(this->window, nullptr, &h);\n SDL_SetWindowSize(this->window, w, h);",
             "int h;\n\t\tFateGetWinSize(this->window, nullptr, &h);\n\t\tFateSetWinSize(this->window, w, h);", marker="FateGetWinSize(this->window, nullptr, &h);\n\t\tFateSetWinSize"),
        Edit("int w;\n SDL_GetWindowSize(this->window, &w, nullptr);\n SDL_SetWindowSize(this->window, w, h);",
             "int w;\n\t\tFateGetWinSize(this->window, &w, nullptr);\n\t\tFateSetWinSize(this->window, w, h);", marker="FateGetWinSize(this->window, &w, nullptr);\n\t\tFateSetWinSize"),
        Edit("if (this->window)\n {\n SDL_SetWindowSize(this->window, w, h);",
             "if (this->window)\n\t{\n\t\tFateSetWinSize(this->window, w, h);", marker="{\n\t\tFateSetWinSize(this->window, w, h);"),
        Edit("SDL_GetWindowSize(this->window, &w, &h);\n return;",
             "FateGetWinSize(this->window, &w, &h);\n\t\treturn;", marker="FateGetWinSize(this->window, &w, &h);"),
        Edit("SDL_GetWindowSize(this->window, &w, nullptr);\n return w;",
             "FateGetWinSize(this->window, &w, nullptr);\n\t\treturn w;", marker="FateGetWinSize(this->window, &w, nullptr);\n\t\treturn w;"),
        Edit("SDL_GetWindowSize(this->window, nullptr, &h);\n return h;",
             "FateGetWinSize(this->window, nullptr, &h);\n\t\treturn h;", marker="FateGetWinSize(this->window, nullptr, &h);\n\t\treturn h;"),
        Edit("case SDL_WINDOWEVENT_RESIZED:\n case SDL_WINDOWEVENT_SIZE_CHANGED:\n {",
             "case SDL_WINDOWEVENT_RESIZED:\n\t\t\t\t\t\tcase SDL_WINDOWEVENT_SIZE_CHANGED:\n\t\t\t\t\t\t{\n\t\t\t\t\t\t\tgFateReqSize.erase(this->window);",
             marker="gFateReqSize.erase(this->window);"),
      ],
      # diagnostics for resize problems (what size the game asks for, what the paint box becomes)
      "src/core/sdl2/SDLApplication.cpp#11": [
        Edit("void TVPWindowWindow::SetPaintBoxSize(tjs_int w, tjs_int h)\n{",
             "void TVPWindowWindow::SetPaintBoxSize(tjs_int w, tjs_int h)\n{\n\t{ char m[80]; snprintf(m, sizeof(m), \"paint box %dx%d\", (int)w, (int)h); extern void TVPFateDebugLog(const char*); TVPFateDebugLog(m); }",
             marker="\"paint box %dx%d\""),
        Edit("void TVPWindowWindow::SetZoom(tjs_int numer, tjs_int denom, bool set_logical)\n{",
             "void TVPWindowWindow::SetZoom(tjs_int numer, tjs_int denom, bool set_logical)\n{\n\t{ char m[80]; snprintf(m, sizeof(m), \"setZoom %d/%d logical %d\", (int)numer, (int)denom, (int)set_logical); extern void TVPFateDebugLog(const char*); TVPFateDebugLog(m); }",
             marker="\"setZoom %d/%d logical %d\""),
        Edit("void TVPWindowWindow::SetFullScreenMode(bool fullscreen)\n{",
             "void TVPWindowWindow::SetFullScreenMode(bool fullscreen)\n{\n\t{ extern void TVPFateDebugLog(const char*); TVPFateDebugLog(fullscreen ? \"fullscreen on\" : \"fullscreen off\"); }",
             marker="\"fullscreen on\" : \"fullscreen off\""),
        Edit("void TVPWindowWindow::SetInnerSize(tjs_int w, tjs_int h)\n{",
             "void TVPWindowWindow::SetInnerSize(tjs_int w, tjs_int h)\n{\n\t{ char m[80]; snprintf(m, sizeof(m), \"setInnerSize %dx%d\", (int)w, (int)h); extern void TVPFateDebugLog(const char*); TVPFateDebugLog(m); }",
             marker="\"setInnerSize %dx%d\""),
      ],
      "src/core/sdl2/SDLApplication.cpp#12": [
        Edit("\ttjs_int FateZoomNumer = 1, FateZoomDenom = 1;   // window zoom as set by the game",
             "\ttjs_int FateZoomNumer = 1, FateZoomDenom = 1;   // window zoom as set by the game\n"
             "\ttjs_int FateViewW = 0, FateViewH = 0;   // part of the primary layer the game's window shows (its inner size without zoom); 0 = unknown\n"
             "\tvoid FateApplyLogical();",
             marker="FateApplyLogical();"),
        Edit("#else\n\t\tSDL_RenderSetLogicalSize(this->renderer, w, h);\n#endif\n\t}\n\tif (this->TJSNativeInstance)",
             "#else\n\t\tthis->FateApplyLogical();\n#endif\n\t}\n\tif (this->TJSNativeInstance)",
             marker="this->FateApplyLogical();"),
        Edit("void TVPWindowWindow::SetPaintBoxSize(tjs_int w, tjs_int h)\n{",
             "// The primary layer can be larger than the window (wide screen: the layer keeps the 4:3 height of its backgrounds, the\n"
             "// window shows the top part, like Kirikiri's paint box). Show only the window's part so the picture is not squeezed.\n"
             "void TVPWindowWindow::FateApplyLogical()\n{\n"
             "\tif (!this->renderer || !this->surface) return;\n"
             "\tint vw = this->surface->w, vh = this->surface->h;\n"
             "\tif (this->FateViewW > 0 && this->FateViewW < vw) vw = this->FateViewW;\n"
             "\tif (this->FateViewH > 0 && this->FateViewH < vh) vh = this->FateViewH;\n"
             "\tSDL_RenderSetLogicalSize(this->renderer, vw, vh);\n}\n\n"
             "void TVPWindowWindow::SetPaintBoxSize(tjs_int w, tjs_int h)\n{",
             marker="void TVPWindowWindow::FateApplyLogical()"),
        Edit("\tthis->SetSize(w, h);\n}\n\ntjs_int TVPWindowWindow::GetInnerWidth()",
             "\tif (this->FateZoomNumer > 0 && this->FateZoomDenom > 0)\n\t{\n"
             "\t\tthis->FateViewW = (tjs_int)((long long)w * this->FateZoomDenom / this->FateZoomNumer);\n"
             "\t\tthis->FateViewH = (tjs_int)((long long)h * this->FateZoomDenom / this->FateZoomNumer);\n\t}\n"
             "\tthis->SetSize(w, h);\n\tthis->FateApplyLogical();\n}\n\ntjs_int TVPWindowWindow::GetInnerWidth()",
             marker="this->FateViewW = (tjs_int)"),
        Edit("\t\t\t\t\tif (gFateFullPresent) SDL_RenderCopy(this->renderer, this->texture, nullptr, nullptr);",
             "\t\t\t\t\tif (gFateFullPresent)\n\t\t\t\t\t{\n\t\t\t\t\t\tSDL_Rect fsrc = {0, 0, logical_rect.w, logical_rect.h};\n"
             "\t\t\t\t\t\tif (fsrc.w > 0 && fsrc.h > 0) SDL_RenderCopy(this->renderer, this->texture, &fsrc, nullptr);\n"
             "\t\t\t\t\t\telse SDL_RenderCopy(this->renderer, this->texture, nullptr, nullptr);\n\t\t\t\t\t}",
             marker="SDL_Rect fsrc"),
      ],
      "src/core/sdl2/SDLApplication.cpp#13": [
        Edit("#else\n\tthis->SetWidth(v);\n#endif\n}\n\nvoid TVPWindowWindow::SetInnerHeight",
             "#else\n\tthis->SetInnerSize(v, this->GetInnerHeight());   // keeps the visible part of the layer up to date\n#endif\n}\n\nvoid TVPWindowWindow::SetInnerHeight",
             marker="keeps the visible part of the layer up to date"),
        Edit("#else\n\tthis->SetHeight(v);\n#endif\n}\n\nvoid TVPWindowWindow::SetInnerSize",
             "#else\n\tthis->SetInnerSize(this->GetInnerWidth(), v);   // keeps the visible part of the layer up to date\n#endif\n}\n\nvoid TVPWindowWindow::SetInnerSize",
             marker="SetInnerSize(this->GetInnerWidth(), v);   // keeps"),
      ],
      "src/core/sdl2/SDLApplication.cpp#9": [
        Edit("void TVPWindowWindow::TickBeat()\n{",
             "void TVPWindowWindow::TickBeat()\n{\n\tif (FateMovieOverlayDirty() && this->bitmapCompletion && this->surface)\n\t{\n"
             "\t\tthis->bitmapCompletion->update_rect = tTVPRect(0, 0, this->surface->w, this->surface->h);\n\t\tthis->needsGraphicUpdate = true;\n\t}",
             marker="FateMovieOverlayDirty()"),
        Edit("\t\t\t\tSDL_RenderPresent(this->renderer);\n#if !defined(KRKRSDL2_ENABLE_ZOOM) && !defined(KRKRSDL2_RENDERER_FULL_UPDATES)\n\t\t\t\tif (!gFateFullPresent",
             "\t\t\t\tFateMovieDrawOverlay(this->renderer, this->surface ? this->surface->w : 0);\n"
             "\t\t\t\tSDL_RenderPresent(this->renderer);\n#if !defined(KRKRSDL2_ENABLE_ZOOM) && !defined(KRKRSDL2_RENDERER_FULL_UPDATES)\n\t\t\t\tif (!gFateFullPresent",
             marker="FateMovieDrawOverlay(this->renderer"),
        Edit("static bool gFateFullPresent", "#include \"fate_video.h\"\nstatic bool gFateFullPresent", marker="#include \"fate_video.h\""),
      ],
    }

EDITS.update(video_edits())

def copy_overlay(root, check):
    n = 0
    for dp, _, fs in os.walk(OVERLAY):
        for f in fs:
            src = os.path.join(dp, f)
            dst = os.path.join(root, os.path.relpath(src, OVERLAY))
            if os.path.exists(dst) and open(src, "rb").read() == open(dst, "rb").read():
                continue
            n += 1
            if not check:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
    return n

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check = "--check" in sys.argv
    if len(args) != 1:
        print(__doc__); return 2
    root = os.path.abspath(args[0])
    if not os.path.exists(os.path.join(root, "src/core/sdl2/SDLApplication.cpp")):
        print("error: not a krkrsdl2 checkout:", root); return 2
    if not os.path.exists(os.path.join(root, "external/krkrz/visual/FreeType.cpp")):
        print("error: external/krkrz missing - clone with --recursive / git submodule update --init"); return 2
    bad = 0
    for rel, edits in EDITS.items():
        try:
            c = apply_edits(root, rel, edits, check)
            print(("would patch " if check and c else "patched     " if c else "up to date  ") + rel)
        except Drift as d:
            bad += 1
            print("DRIFT       " + str(d))
    n = copy_overlay(root, check)
    print(f"{'would copy' if check else 'copied'}      {n} overlay file(s)")
    if bad:
        print(f"\n{bad} upstream anchor(s) changed - update tools/fate-port.py (EDITS) for the new upstream code.")
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
