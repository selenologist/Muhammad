// SPDX-License-Identifier: GPL-3.0-only
// clap-gui backend, Windows: a WS_CHILD window embedded in the host's window
// via SetParent, WGL + NanoVG GL3 rendering the shared p5ui panel. All
// platform-neutral state and semantics live in gui_core.{hpp,cpp}; this file
// creates the window + GL context and translates Win32 messages into the
// neutral input API.
//
// Linking note: mingw's libopengl32.a exports only the GL 1.1 entry points,
// but <GL/glext.h> prototypes (GL_GLEXT_PROTOTYPES=1, from the nanovg target)
// let nanovg call 1.2+/2.0/3.x functions directly. For every such function we
// define an extern "C" thunk below that lazily resolves the real address via
// wglGetProcAddress (fallback: opengl32.dll for 1.1-style names) after a GL
// context exists. The mingw linker's undefined-reference list is the authority
// on completeness; these thunks cover exactly the calls nanovg's gl3_impl makes.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>

#include <nanovg.h>
#define NANOVG_GL3 1
#include <nanovg_gl.h>

#include <clap/clap.h>
#include <clap/ext/timer-support.h>

#include <cstdio>
#include <cstring>

#include "gui_core.hpp"
#include "muhammad_plugin.hpp"
#include "p5_gui.hpp"

namespace pw {

namespace {

// ---- GL >= 1.2 thunk table ---------------------------------------------------

void *glProc(const char *n)
{   // wglGetProcAddress must be called with a current context (it is: every
    // thunk first fires from a rendered frame). It returns (PROC)1..3 on the
    // "extension not supported" error path, NULL for GL 1.1 functions.
    if (void *p = (void *)(INT_PTR)wglGetProcAddress(n))
    {   if ((INT_PTR)p > 3) return p; }
    return (void *)(INT_PTR)GetProcAddress(GetModuleHandleA("opengl32.dll"), n);
}

#define GLTHUNK(ret, name, params, args) \
    static ret (APIENTRY *p_##name) params = nullptr; \
    extern "C" ret APIENTRY name params \
    {   if (!p_##name) p_##name = (ret (APIENTRY *) params)glProc(#name); \
        if (p_##name) return p_##name args; \
        return (ret)0; }   // legal also for void: 'return (void)0;'

// texture units / programs / shaders (1.3, 2.0)
GLTHUNK(void, glActiveTexture, (GLenum texture), (texture))
GLTHUNK(void, glAttachShader, (GLuint program, GLuint shader), (program, shader))
GLTHUNK(void, glBindAttribLocation, (GLuint program, GLuint index, const GLchar *name), (program, index, name))
GLTHUNK(void, glBlendFuncSeparate, (GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha), (srcRGB, dstRGB, srcAlpha, dstAlpha))
GLTHUNK(GLuint, glCreateProgram, (void), ())
GLTHUNK(GLuint, glCreateShader, (GLenum type), (type))
GLTHUNK(void, glDeleteProgram, (GLuint program), (program))
GLTHUNK(void, glDeleteShader, (GLuint shader), (shader))
GLTHUNK(void, glLinkProgram, (GLuint program), (program))
GLTHUNK(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length), (shader, count, string, length))
GLTHUNK(void, glCompileShader, (GLuint shader), (shader))
GLTHUNK(void, glUseProgram, (GLuint program), (program))
GLTHUNK(GLint, glGetUniformLocation, (GLuint program, const GLchar *name), (program, name))
// shaders/programs queries
GLTHUNK(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params), (shader, pname, params))
GLTHUNK(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog), (shader, bufSize, length, infoLog))
GLTHUNK(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params), (program, pname, params))
GLTHUNK(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog), (program, bufSize, length, infoLog))
// buffers (1.4/1.5)
GLTHUNK(void, glGenBuffers, (GLsizei n, GLuint *buffers), (n, buffers))
GLTHUNK(void, glBindBuffer, (GLenum target, GLuint buffer), (target, buffer))
GLTHUNK(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage), (target, size, data, usage))
GLTHUNK(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers), (n, buffers))
// vertex arrays (2.0/3.0)
GLTHUNK(void, glEnableVertexAttribArray, (GLuint index), (index))
GLTHUNK(void, glDisableVertexAttribArray, (GLuint index), (index))
GLTHUNK(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer), (index, size, type, normalized, stride, pointer))
GLTHUNK(void, glGenVertexArrays, (GLsizei n, GLuint *arrays), (n, arrays))
GLTHUNK(void, glBindVertexArray, (GLuint array), (array))
GLTHUNK(void, glDeleteVertexArrays, (GLsizei n, const GLuint *arrays), (n, arrays))
// uniforms (2.0)
GLTHUNK(void, glUniform1i, (GLint location, GLint v0), (location, v0))
GLTHUNK(void, glUniform2fv, (GLint location, GLsizei count, const GLfloat *value), (location, count, value))
GLTHUNK(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat *value), (location, count, value))
// uniform blocks (3.0/3.1)
GLTHUNK(GLuint, glGetUniformBlockIndex, (GLuint program, const GLchar *uniformBlockName), (program, uniformBlockName))
GLTHUNK(void, glUniformBlockBinding, (GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding), (program, uniformBlockIndex, uniformBlockBinding))
GLTHUNK(void, glBindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size), (target, index, buffer, offset, size))
// misc (1.4/3.0)
GLTHUNK(void, glGenerateMipmap, (GLenum target), (target))
GLTHUNK(void, glStencilOpSeparate, (GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass), (face, sfail, dpfail, dppass))

// ---- embedding context --------------------------------------------------------

// ---- CF_UNICODETEXT clipboard (debug COPY) ---------------------------------
namespace clip {
HWND hwnd = nullptr;                 // set by setupWin32
}

static bool win32CopyText(const char *t)
{
    if (!clip::hwnd) return false;
    const int wn = MultiByteToWideChar(CP_UTF8, 0, t, -1, nullptr, 0);
    if (wn <= 0) return false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (size_t)wn * sizeof(wchar_t));
    if (!mem) return false;
    auto *dst = static_cast<wchar_t *>(GlobalLock(mem));
    MultiByteToWideChar(CP_UTF8, 0, t, -1, dst, wn);
    GlobalUnlock(mem);
    if (!OpenClipboard(clip::hwnd)) { GlobalFree(mem); return false; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, mem)) { GlobalFree(mem); CloseClipboard(); return false; }
    CloseClipboard();                              // ownership transferred
    return true;
}

struct Ctx : GuiCore
{
    explicit Ctx(Plugin *p) : GuiCore(p) {}

    HWND parent = nullptr, win = nullptr;
    HDC hdc = nullptr;
    HGLRC gl = nullptr;
    NVGcontext *vg = nullptr;
    p5ui::Gui gui{};
    bool hasGui = false;
    bool captured = false;              // SetCapture is in flight
};

Ctx *ctxFrom(const clap_plugin_t *p)
{
    auto *s = static_cast<Plugin *>(p->plugin_data);
    return s ? static_cast<Ctx *>(s->gui) : nullptr;
}

void paintNow(Ctx &c)
{
    if (!c.win || !c.vg || !c.mapped) return;
    wglMakeCurrent(c.hdc, c.gl);
    glViewport(0, 0, (int)c.w, (int)c.h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    renderFrame(c, c.vg, c.gui);
    SwapBuffers(c.hdc);
}

void afterPress(Ctx &c)
{   // pointer grab when a core drag armed + keyboard focus when a text modal
    // opened — the Win32 mirror of the X11 shim's afterPress().
    if (!c.captured &&
        (c.dragKnob >= 0 || c.trimDrag || c.tuneDrag || c.wheelDragIdx))
    {   SetCapture(c.win); c.captured = true; }
    if (c.needFocus)
    {   SetFocus(c.win); c.needFocus = false; }
}

unsigned mkMods(WPARAM wParam)
{   return (wParam & MK_SHIFT) ? P5_MOD_SHIFT : 0u;
}

unsigned keyMods()
{   return (GetKeyState(VK_SHIFT) & 0x8000u) ? P5_MOD_SHIFT : 0u;
}

// ---- window class ---------------------------------------------------------------

constexpr const wchar_t *kClassName = L"MuhammadPanelWindowClass";
static HINSTANCE g_inst = nullptr;

LRESULT CALLBACK panelWndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {   auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, msg, wParam, lParam);
    }
    auto *cp = reinterpret_cast<Ctx *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!cp) return DefWindowProcW(h, msg, wParam, lParam);
    Ctx &c = *cp;
    bool dirty = false;

    switch (msg)
    {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:            // core re-detects dbl itself (450 ms)
            dirty = onPointerPress(c, (float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam),
                                   mkMods(wParam), (unsigned)GetTickCount());
            afterPress(c);
            break;
        case WM_LBUTTONUP:
            if (c.captured) { ReleaseCapture(); c.captured = false; }
            dirty = onPointerRelease(c, (float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam),
                                     mkMods(wParam));
            afterPress(c);                // actuate() may have opened a text modal
            break;
        case WM_RBUTTONDOWN:
            dirty = onPointerPress(c, (float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam),
                                   P5_MOD_RIGHT, (unsigned)GetTickCount());
            break;
        case WM_MOUSEMOVE:
            dirty = onPointerMotion(c, (float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam),
                                    mkMods(wParam));
            break;
        case WM_MOUSEWHEEL:
        {   POINT pt{ (short)LOWORD(lParam), (short)HIWORD(lParam) };
            ScreenToClient(h, &pt);
            const int dz = (int)GET_WHEEL_DELTA_WPARAM(wParam);
            dirty = onWheel(c, (float)pt.x, (float)pt.y, dz < 0 ? +1 : -1);   // down = next page
            break;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        {   GuiKey k = KEY_NONE;
            switch ((int)wParam)
            {   case VK_ESCAPE:    k = K_ESCAPE; break;
                case VK_BACK:      k = K_BACKSPACE; break;
                case VK_RETURN:    k = K_RETURN; break;
                case VK_UP:        k = K_UP; break;
                case VK_DOWN:      k = K_DOWN; break;
                case VK_LEFT:      k = K_LEFT; break;
                case VK_RIGHT:     k = K_RIGHT; break;
                default: break; }
            if (onKey(c, k, keyMods())) dirty = true;
            break;
        }
        case WM_CHAR:
            // onChar re-applies the same allowlist as X11's XLookupString path;
            // Return/Escape/Backspace/arrows already fell out as non-allowed.
            dirty = onChar(c, (unsigned int)(wchar_t)wParam);
            break;
        case WM_SIZE:
        {   const uint32_t nw = (uint32_t)(unsigned)LOWORD(lParam);
            const uint32_t nh = (uint32_t)(unsigned)HIWORD(lParam);
            if (nw > 0 && nh > 0 && (nw != c.w || nh != c.h))
            {   c.w = nw; c.h = nh;
                c.resizeRequests = 0;     // host answered (or user dragged): renegotiate
                paintNow(c);
            }
            return 0;
        }
        case WM_SETFOCUS:
            return 0;
        case WM_PAINT:
        {   PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            ValidateRect(h, nullptr);
            paintNow(c);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;                     // GL paints everything: no flicker
        default:
            return DefWindowProcW(h, msg, wParam, lParam);
    }
    if (dirty) paintNow(c);
    return 0;
}

bool ensureClass()
{
    static bool done = false;
    if (done) return true;
    if (!g_inst)
    {   HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&g_inst, &m);
        g_inst = (HINSTANCE)m;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC | CS_DBLCLKS;      // CS_OWNDC: one HDC per window (wgl-safe)
    wc.lpfnWndProc = panelWndProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;            // we own every pixel via GL
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    done = true;
    return true;
}

// ---- pump (timer-support) --------------------------------------------------------

void pump(Ctx &c)
{
    if (!c.win) return;
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
    {   TranslateMessage(&m);
        DispatchMessageW(&m); }
    bool dirty = false;
    // Never copy a too-tall slot: coreMirrorSlot owns the aspect math; we
    // just read the host's client size and apply the answer.
    if (c.parent)
    {   RECT pr{};
        if (GetClientRect(c.parent, &pr))
        {   uint32_t ow = 0, oh = 0;
            if (coreMirrorSlot(c, (int)(pr.right - pr.left), (int)(pr.bottom - pr.top), ow, oh))
            {   MoveWindow(c.win, 0, 0, (int)ow, (int)oh, TRUE);
                dirty = true; }
        }
    }
    // Measure wrapper + ancestor every ~1s (diag) and politely ask the host
    // for the ancestor's height, exactly like the X11 shim.
    {   const long t = (long)GetTickCount();
        if (t - c.lastAskMs >= 1000 && c.parent)
        {   RECT pr{}, gr{};
            if (GetClientRect(c.parent, &pr))
            {   uint32_t ancW = 0, ancH = 0;
                HWND gp = GetParent(c.parent);
                if (gp && GetClientRect(gp, &gr))
                {   ancW = (uint32_t)(gr.right - gr.left); ancH = (uint32_t)(gr.bottom - gr.top); }
                coreAncestorAsk(c, (uint32_t)(pr.right - pr.left), (uint32_t)(pr.bottom - pr.top),
                                ancW, ancH, t);
            }
        }
    }
    if (dirty) paintNow(c);
}

// ---- lifecycle ---------------------------------------------------------------------

void teardown(Ctx &c)
{
    if (c.vg && c.gl) wglMakeCurrent(c.hdc, c.gl);   // thunks need a live ctx
    if (c.vg) { nvgDeleteGL3(c.vg); c.vg = nullptr; c.hasGui = false; }
    wglMakeCurrent(nullptr, nullptr);
    if (c.gl) { wglDeleteContext(c.gl); c.gl = nullptr; }
    if (c.win && c.hdc) ReleaseDC(c.win, c.hdc);
    c.hdc = nullptr;
    if (c.win) { DestroyWindow(c.win); c.win = nullptr; }
    c.mapped = false;
}

bool setupWin32(Ctx &c, HWND parent)
{
    c.parent = parent;
    if (!ensureClass()) return false;
    uint32_t w, h;
    if (c.reqW && c.reqH) { w = c.reqW; h = c.reqH; }          // host already told us
    else
    {
        w = (uint32_t)(p5ui::kPanelW * (c.scale > 0.0 ? c.scale : 1.0) + 0.5);
        h = (uint32_t)(w * p5ui::kPanelH / p5ui::kPanelW);
    }
    c.w = w; c.h = h;

    c.win = CreateWindowExW(0, kClassName, L"Muhammad", WS_CHILD | WS_CLIPCHILDREN,
                            0, 0, (int)w, (int)h, parent, nullptr, g_inst, &c);
    if (!c.win) return false;
    clip::hwnd = c.win;
    SetWindowLongPtrW(c.win, GWL_STYLE, WS_CHILD | WS_CLIPCHILDREN);
    SetWindowPos(c.win, nullptr, 0, 0, (int)w, (int)h, SWP_NOZORDER | SWP_NOACTIVATE);
    std::fprintf(stderr, "[p5gui] win32 child window created: %p parent %p size %ux%u\n",
                 (void *)c.win, (void *)c.parent, c.w, c.h);

    c.hdc = GetDC(c.win);
    if (!c.hdc) { teardown(c); return false; }

    // Seed a basic double-buffered format so wglCreateContext gives us a
    // temporary context to query the ARB entry points through.
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24; pfd.cAlphaBits = 8; pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int basePf = ChoosePixelFormat(c.hdc, &pfd);
    if (!basePf || !SetPixelFormat(c.hdc, basePf, &pfd)) { teardown(c); return false; }
    HGLRC tmp = wglCreateContext(c.hdc);
    if (!tmp || !wglMakeCurrent(c.hdc, tmp)) { teardown(c); return false; }

    auto chooseARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)(INT_PTR)wglGetProcAddress("wglChoosePixelFormatARB");
    auto createCtxARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)(INT_PTR)wglGetProcAddress("wglCreateContextAttribsARB");

    if (chooseARB)
    {   const int fbAttrs[] = {
            WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1,
            WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
            WGL_COLOR_BITS_ARB, 32, WGL_ALPHA_BITS_ARB, 8, WGL_DEPTH_BITS_ARB, 0,
            WGL_STENCIL_BITS_ARB, 8, 0 };
        int fmt = 0; UINT n = 0;
        if (chooseARB(c.hdc, fbAttrs, nullptr, 1, &fmt, &n) && n >= 1 && fmt != 0)
        {   wglMakeCurrent(c.hdc, nullptr);
            wglDeleteContext(tmp); tmp = nullptr;
            if (!SetPixelFormat(c.hdc, fmt, nullptr)) { teardown(c); return false; }
        }
    }

    if (createCtxARB)
    {   const int core33[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                               WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
        const int compat31[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 1,
                                 WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, 0 };
        c.gl = createCtxARB(c.hdc, nullptr, core33);
        if (!c.gl) c.gl = createCtxARB(c.hdc, nullptr, compat31);
    }
    if (!c.gl) c.gl = tmp ? tmp : wglCreateContext(c.hdc);     // legacy 1.1 fallback
    else if (tmp && tmp != c.gl) wglDeleteContext(tmp);
    if (!c.gl) { teardown(c); return false; }
    if (!wglMakeCurrent(c.hdc, c.gl)) { teardown(c); return false; }
    std::fprintf(stderr, "[p5gui] GL: %s\n", (const char *)glGetString(GL_VERSION));

    c.vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!c.vg)
    {   std::fprintf(stderr, "[p5gui] win32: GL 3.x unavailable (nanovg GL3 init failed) - unsupported\n");
        teardown(c); return false; }
    c.gui = p5ui::create(c.vg);
    c.hasGui = c.gui.fontId >= 0;
    return c.hasGui;
}

// ---- clap_plugin_gui surface ---------------------------------------------------------

bool gui_is_api_supported(const clap_plugin_t *, const char *api, bool is_floating)
{   // a win32 build answers win32 only
    return !is_floating && api && !std::strcmp(api, CLAP_WINDOW_API_WIN32);
}
bool gui_get_preferred_api(const clap_plugin_t *, const char **api, bool *is_floating)
{
    *api = CLAP_WINDOW_API_WIN32; *is_floating = false; return true;
}
bool gui_create(const clap_plugin_t *p, const char *api, bool is_floating)
{
    std::fprintf(stderr, "[p5gui] create api=%s floating=%d\n", api ? api : "(null)", (int)is_floating);
    if (!gui_is_api_supported(p, api, is_floating)) return false;
    if (auto *cc = ctxFrom(p)) if (api) std::strncpy(cc->guiApi, api, sizeof cc->guiApi - 1);
    auto *s = static_cast<Plugin *>(p->plugin_data);
    if (!s->gui)
    {
        s->gui = new Ctx(s);
        Ctx &c = *static_cast<Ctx *>(s->gui);
        c.copyText = win32CopyText;
        if (s->host && s->host->get_extension)
            c.hostGui = (const clap_host_gui_t *)s->host->get_extension(s->host, CLAP_EXT_GUI);
        std::fprintf(stderr, "[p5gui] host clap.gui service %s\n", c.hostGui ? "present" : "absent");
    }
    return true;
}
void gui_destroy(const clap_plugin_t *p)
{
    auto *s = static_cast<Plugin *>(p->plugin_data);
    if (!s->gui) return;
    Ctx &c = *static_cast<Ctx *>(s->gui);
    unregisterTimer(c);
    teardown(c);
    delete &c;
    s->gui = nullptr;
}
bool gui_set_scale(const clap_plugin_t *p, double scale)
{
    std::fprintf(stderr, "[p5gui] set_scale %g\n", scale);
    // win32 uses PHYSICAL pixels: honor the host scale (layout * scale).
    if (auto *c = ctxFrom(p)) { c->scale = scale; return true; }
    return false;
}
bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
    double scale = 1.0;
    if (auto *c = ctxFrom(p)) if (c->scale > 0.0) scale = c->scale;
    *w = (uint32_t)(p5ui::kPanelW * scale + 0.5);
    *h = (uint32_t)(p5ui::kPanelH * scale + 0.5);
    return true;
}
bool gui_can_resize(const clap_plugin_t *) { return true; }
bool gui_get_resize_hints(const clap_plugin_t *, clap_gui_resize_hints_t *hints)
{
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = true;
    hints->aspect_ratio_width = (uint32_t)p5ui::kPanelW;
    hints->aspect_ratio_height = (uint32_t)p5ui::kPanelH;
    return true;
}
bool gui_adjust_size(const clap_plugin_t *, uint32_t *w, uint32_t *h)
{
    uint32_t nw = *w < 520u ? 520u : (*w > 3120u ? 3120u : *w);
    *w = nw; *h = (uint32_t)(nw * p5ui::kPanelH / p5ui::kPanelW);
    return true;
}
bool gui_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h)
{
    std::fprintf(stderr, "[p5gui] set_size %ux%u\n", w, h);
    auto *c = ctxFrom(p);
    if (!c) return false;
    // The host's size is authoritative: resize EXACTLY to it (aspect belongs
    // to the drag hints, not to set_size — same reasoning as gui_x11.cpp).
    if (w == 0 || h == 0) return false;
    c->reqW = w; c->reqH = h;
    if (c->win)
    {
        MoveWindow(c->win, 0, 0, (int)w, (int)h, TRUE);   // WM_SIZE sets w/h + repaints
        c->w = w; c->h = h;
        paintNow(*c);
    }
    return true;
}
bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *window)
{
    std::fprintf(stderr, "[p5gui] set_parent api=%s\n", window ? window->api : "(null)");
    auto *c = ctxFrom(p);
    if (!c || !window || std::strcmp(window->api, CLAP_WINDOW_API_WIN32) != 0) return false;
    return setupWin32(*c, (HWND)window->win32);
}
bool gui_set_transient(const clap_plugin_t *, const clap_window_t *) { return false; }
void gui_suggest_title(const clap_plugin_t *, const char *) {}
bool gui_show(const clap_plugin_t *p)
{
    std::fprintf(stderr, "[p5gui] show\n");
    auto *c = ctxFrom(p);
    if (!c || !c->win) return false;
    ShowWindow(c->win, SW_SHOW);
    c->mapped = true;
    registerTimer(*c);
    paintNow(*c);
    return true;
}
bool gui_hide(const clap_plugin_t *p)
{
    auto *c = ctxFrom(p);
    if (!c || !c->win) return false;
    unregisterTimer(*c);
    ShowWindow(c->win, SW_HIDE);
    c->mapped = false;
    return true;
}

} // namespace

const clap_plugin_gui_t kGui = {
    gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy,
    gui_set_scale, gui_get_size, gui_can_resize, gui_get_resize_hints,
    gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient,
    gui_suggest_title, gui_show, gui_hide,
};

// timer-support: pump the Win32 message queue + repaint (hosts that fire
// timer-support get the same behaviour as the X11 pump; hosts that only pump
// via WM_PAINT still get frames through WndProc).
static void gui_on_timer(const clap_plugin_t *p, clap_id)
{
    if (auto *c = ctxFrom(p)) { pump(*c); }
}
const clap_plugin_timer_support_t kTimerSupport = { gui_on_timer };

} // namespace pw
