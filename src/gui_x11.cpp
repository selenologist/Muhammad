// SPDX-License-Identifier: GPL-3.0-only
// clap-gui backend: an X11 child window embedded in the host's window, GLX +
// NanoVG GL3 rendering the shared p5ui panel, mouse gestures back into CLAP
// params (via Plugin::guiSet, drained to the host on the next params.flush).
// All platform-neutral state and semantics live in gui_core.{hpp,cpp}; this
// file only creates the window + GL context and translates XEvents into the
// neutral input API.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <GL/glx.h>
#include <GL/gl.h>

#include <nanovg.h>
#define NANOVG_GL3 1
#include <nanovg_gl.h>

#include <clap/clap.h>
#include <clap/ext/timer-support.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "gui_core.hpp"
#include "muhammad_plugin.hpp"
#include "p5_gui.hpp"

namespace pw {

namespace {

// ---- CLIPBOARD selection owner (debug COPY) -------------------------------
namespace clip {
Display *dpy = nullptr;                 // set by setupEmbedded (one GUI per process)
Window win = 0;
char text[1200] = {};
int len = 0;
Atom aCLIPBOARD = None, aUTF8 = None, aTARGETS = None;
}

static bool x11CopyText(const char *t)
{
    if (!clip::dpy || !clip::win) return false;
    const size_t n = std::strlen(t);
    if (n >= sizeof clip::text) return false;
    std::memcpy(clip::text, t, n + 1);
    clip::len = (int)n;
    if (!clip::aCLIPBOARD)
    {   clip::aCLIPBOARD = XInternAtom(clip::dpy, "CLIPBOARD", False);
        clip::aUTF8      = XInternAtom(clip::dpy, "UTF8_STRING", False);
        clip::aTARGETS   = XInternAtom(clip::dpy, "TARGETS", False); }
    XSetSelectionOwner(clip::dpy, clip::aCLIPBOARD, clip::win, CurrentTime);
    if (XGetSelectionOwner(clip::dpy, clip::aCLIPBOARD) != clip::win)
    {   std::fprintf(stderr, "[p5gui] CLIPBOARD: could not acquire ownership\n"); return false; }
    return true;
}

static void serveSelectionRequest(const XSelectionRequestEvent &rq)
{
    if (rq.selection != clip::aCLIPBOARD || rq.property == None) return;
    XEvent nt{};
    nt.xselection.type = SelectionNotify;
    nt.xselection.requestor = rq.requestor;
    nt.xselection.selection = rq.selection;
    nt.xselection.target = rq.target;
    nt.xselection.property = rq.property;
    nt.xselection.time = rq.time;
    if (rq.target == clip::aTARGETS)
    {   const Atom list[2] = { clip::aTARGETS, clip::aUTF8 };
        XChangeProperty(clip::dpy, rq.requestor, rq.property, XA_ATOM, 32,
                        PropModeReplace, (const unsigned char *)list, 2); }
    else if (rq.target == clip::aUTF8 || rq.target == XA_STRING)
    {   XChangeProperty(clip::dpy, rq.requestor, rq.property, rq.target, 8,
                        PropModeReplace, (const unsigned char *)clip::text, clip::len); }
    else
    {   nt.xselection.property = None; }          // unsupported target
    XSendEvent(clip::dpy, rq.requestor, False, 0, &nt);
}

struct Ctx : GuiCore
{
    explicit Ctx(Plugin *p) : GuiCore(p) {}

    Display *dpy = nullptr;
    Window parent = 0, win = 0;
    Atom xembed = None, xembedInfo = None;
    GLXContext gl = nullptr;
    Atom wmDelete = None;
    NVGcontext *vg = nullptr;
    p5ui::Gui gui{};
    bool hasGui = false;
    bool pointerGrabbed = false;    // XGrabPointer is in flight
};

Ctx *ctxFrom(const clap_plugin_t *p)
{
    auto *s = static_cast<Plugin *>(p->plugin_data);
    return s ? static_cast<Ctx *>(s->gui) : nullptr;
}

// ---- draw / pump -------------------------------------------------------------

void render(Ctx &c)
{
    if (!c.vg || !c.mapped) return;
    glXMakeCurrent(c.dpy, c.win, c.gl);
    glViewport(0, 0, (int)c.w, (int)c.h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    renderFrame(c, c.vg, c.gui);
    glXSwapBuffers(c.dpy, c.win);
}

long nowMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// grab the pointer when a core drag armed, hand keyboard focus when a text
// modal opened — both were inline X calls before the extraction.
void afterPress(Ctx &c)
{
    if (!c.pointerGrabbed &&
        (c.dragKnob >= 0 || c.trimDrag || c.tuneDrag || c.wheelDragIdx))
    {   XGrabPointer(c.dpy, c.win, True,
                     ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync,
                     None, None, CurrentTime);
        c.pointerGrabbed = true; }
    if (c.needFocus)
    {   XSetInputFocus(c.dpy, c.win, RevertToParent, CurrentTime); c.needFocus = false; }
}

void pump(Ctx &c)
{
    bool dirty = false;
    if (c.parent && c.win)
    {
        XWindowAttributes pa{};
        if (XGetWindowAttributes(c.dpy, c.parent, &pa))
        {
            uint32_t ow, oh;
            if (coreMirrorSlot(c, pa.width, pa.height, ow, oh))
            {   XResizeWindow(c.dpy, c.win, ow, oh);
                dirty = true; }
        }
    }
    // Bitwig sizes our immediate container to the aspect we report; wasted
    // space may live one level up. Measure both every ~1s (diag shows them:
    // a screenshot then proves WHICH window owns any dead band), and while
    // still polite ask the host for the ancestor's height.
    {
        const long t = nowMs();
        if (t - c.lastAskMs >= 1000)
        {
            XWindowAttributes pa{};
            if (c.parent && XGetWindowAttributes(c.dpy, c.parent, &pa))
            {
                Window r, pp = 0, *kids = nullptr; unsigned nk = 0;
                uint32_t ancW = 0, ancH = 0;
                if (XQueryTree(c.dpy, c.parent, &r, &pp, &kids, &nk) && pp)
                {
                    XWindowAttributes ga{};
                    if (XGetWindowAttributes(c.dpy, pp, &ga)) { ancW = ga.width; ancH = ga.height; }
                }
                if (kids) XFree(kids);
                coreAncestorAsk(c, pa.width, pa.height, ancW, ancH, t);
            }
        }
    }
    XEvent ev;
    while (XPending(c.dpy))
    {
        XNextEvent(c.dpy, &ev);
        if (ev.type == SelectionRequest)
        {   serveSelectionRequest(ev.xselectionrequest); continue; }
        if (ev.type == SelectionClear) { clip::len = 0; continue; }
        if (ev.xany.window != c.win) continue;
        switch (ev.type)
        {
            case Expose:
                if (ev.xexpose.count == 0) dirty = true;
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0 &&
                    ((uint32_t)ev.xconfigure.width != c.w || (uint32_t)ev.xconfigure.height != c.h))
                {
                    c.w = (uint32_t)ev.xconfigure.width;
                    c.h = (uint32_t)ev.xconfigure.height;
                    c.resizeRequests = 0;   // host answered (or user dragged): renegotiate
                    dirty = true;
                }
                break;
            case ButtonPress:
                if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
                {   dirty |= onWheel(c, (float)ev.xbutton.x, (float)ev.xbutton.y,
                                     ev.xbutton.button == Button4 ? -1 : +1);
                    break;
                }
                if (ev.xbutton.button == Button3)
                {   dirty |= onPointerPress(c, (float)ev.xbutton.x, (float)ev.xbutton.y,
                                           P5_MOD_RIGHT, (unsigned)ev.xbutton.time);
                    break;
                }
                if (ev.xbutton.button != Button1) break;
                dirty |= onPointerPress(c, (float)ev.xbutton.x, (float)ev.xbutton.y,
                                        ev.xbutton.state, (unsigned)ev.xbutton.time);
                afterPress(c);
                break;
            case ButtonRelease:
                if (ev.xbutton.button != Button1) break;
                if (c.pointerGrabbed) { XUngrabPointer(c.dpy, CurrentTime); c.pointerGrabbed = false; }
                dirty |= onPointerRelease(c, (float)ev.xbutton.x, (float)ev.xbutton.y,
                                          ev.xkey.state);
                afterPress(c);          // actuate() may have opened a text modal
                break;
            case MotionNotify:
                dirty |= onPointerMotion(c, (float)ev.xmotion.x, (float)ev.xmotion.y,
                                         ev.xmotion.state);
                break;
            case ClientMessage:
                if (ev.xclient.message_type == c.xembed) { /* EMBEDDED/FOCUS/CONFIGURE lifecycle */ }
                break;
            case KeyPress:
            {
                char xb[8]; KeySym ks = NoSymbol;
                const int n = XLookupString(&ev.xkey, xb, sizeof xb - 1, &ks, nullptr);
                GuiKey gk = KEY_NONE;
                switch (ks)
                {   case XK_Escape:    gk = K_ESCAPE; break;
                    case XK_BackSpace: gk = K_BACKSPACE; break;
                    case XK_Return: case XK_KP_Enter: gk = K_RETURN; break;
                    case XK_Up: case XK_KP_Up: gk = K_UP; break;
                    case XK_Down: case XK_KP_Down: gk = K_DOWN; break;
                    case XK_Left: case XK_KP_Left: gk = K_LEFT; break;
                    case XK_Right: case XK_KP_Right: gk = K_RIGHT; break;
                    default: break; }
                if (onKey(c, gk, ev.xkey.state)) dirty = true;
                else if (n > 0 && onChar(c, (unsigned char)xb[0])) dirty = true;
                break;
            }
            default:
                break;
        }
    }
    // The whole X backlog is applied and drawn exactly once. Rendering per
    // motion event serialized us on glXSwapBuffers: after a fast drag the
    // stale backlog kept replaying positions (and their blocking swaps) long
    // after release — that was the audible "trail", ours, not the host's.
    if (dirty) render(c);
    // (shrink-negotiation removed: surplus height is now DRAWN chassis, so
    // asking the host to shrink it would fight the ancestor-grow request)
}

// ---- lifecycle ---------------------------------------------------------------

void teardown(Ctx &c)
{
    if (c.vg) { glXMakeCurrent(c.dpy, None, nullptr); nvgDeleteGL3(c.vg); c.vg = nullptr; c.hasGui = false; }
    if (c.gl) { glXDestroyContext(c.dpy, c.gl); c.gl = nullptr; }
    if (c.win && c.dpy) { XDestroyWindow(c.dpy, c.win); c.win = 0; }
    if (c.dpy) { XCloseDisplay(c.dpy); c.dpy = nullptr; }
    c.mapped = false;
}

bool setupEmbedded(Ctx &c, Window parent)
{
    c.parent = parent;                       // pump/mirror/grow all key off this
    c.dpy = XOpenDisplay(nullptr);
    if (!c.dpy) return false;
    clip::dpy = c.dpy;
    const int screen = DefaultScreen(c.dpy);

    static const int fbAttrs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 4, GLX_GREEN_SIZE, 4,
                                   GLX_BLUE_SIZE, 4, GLX_ALPHA_SIZE, 4, GLX_STENCIL_SIZE, 8, None };
    XVisualInfo *vi = glXChooseVisual(c.dpy, screen, (int *)fbAttrs);
    if (!vi) { XCloseDisplay(c.dpy); c.dpy = nullptr; return false; }

    c.gl = glXCreateContext(c.dpy, vi, nullptr, True);
    if (!c.gl) { XFree(vi); XCloseDisplay(c.dpy); c.dpy = nullptr; return false; }

    XSetWindowAttributes swa{};
    swa.colormap = XCreateColormap(c.dpy, parent, vi->visual, AllocNone);
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask | KeyPressMask;   // ClientMessage needs no mask
    uint32_t w, h;
    if (c.reqW && c.reqH) { w = c.reqW; h = c.reqH; }          // host already told us
    else
    {
        w = (uint32_t)(p5ui::kPanelW * 0.75 * (c.scale > 0.0 ? c.scale : 1.0));
        h = (uint32_t)(w * p5ui::kPanelH / p5ui::kPanelW);
    }
    c.w = w; c.h = h;

    c.win = XCreateWindow(c.dpy, parent, 0, 0, (int)w, (int)h, 0, vi->depth, InputOutput, vi->visual,
                          CWColormap | CWEventMask, &swa);
    XFree(vi);
    if (!c.win) { teardown(c); return false; }
    XStoreName(c.dpy, c.win, "Muhammad");
    std::fprintf(stderr, "[p5gui] child window created: 0x%lx parent 0x%lx size %ux%u\n",
                 (unsigned long)c.win, (unsigned long)c.parent, c.w, c.h);
    c.wmDelete = XInternAtom(c.dpy, "WM_DELETE_WINDOW", False);
    clip::win = c.win;

    // Announce XEmbed (the clap spec's x11 embedding protocol) so the host
    // manages us as an embedded window, and pin the aspect in WM hints.
    c.xembed = XInternAtom(c.dpy, "_XEMBED", False);
    c.xembedInfo = XInternAtom(c.dpy, "_XEMBED_INFO", False);
    const long xembedInfoVal[2] = { 0 /*version*/, 1 /*XEMBED_MAPPED*/ };
    XChangeProperty(c.dpy, c.win, c.xembedInfo, XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)xembedInfoVal, 2);
    XClassHint classHint{};
    classHint.res_name = const_cast<char *>("muhammad");
    classHint.res_class = const_cast<char *>("Muhammad");
    XSetClassHint(c.dpy, c.win, &classHint);
    XSizeHints *sh = XAllocSizeHints();
    sh->flags = PAspect | PMinSize;
    sh->min_aspect.x = sh->max_aspect.x = (int)p5ui::kPanelW;   // 1560:416, exactly
    sh->min_aspect.y = sh->max_aspect.y = (int)p5ui::kPanelH;
    sh->min_width = 520; sh->min_height = 139;
    XSetWMNormalHints(c.dpy, c.win, sh);
    XFree(sh);
    {   // XEMBED_EMBEDDED_NOTIFY(0) to the parent
        XEvent ce{};
        ce.xclient.type = ClientMessage;
        ce.xclient.window = parent;
        ce.xclient.message_type = c.xembed;
        ce.xclient.format = 32;
        ce.xclient.data.l[0] = (long)CurrentTime;
        ce.xclient.data.l[1] = 0;               // XEMBED_EMBEDDED_NOTIFY
        ce.xclient.data.l[2] = (long)c.win;
        ce.xclient.data.l[3] = (long)parent;
        ce.xclient.data.l[4] = 0;
        XSendEvent(c.dpy, parent, False, NoEventMask, &ce);
    }

    if (!glXMakeCurrent(c.dpy, c.win, c.gl)) { teardown(c); return false; }
    std::fprintf(stderr, "[p5gui] GL: %s\n", (const char *)glGetString(GL_VERSION));
    c.vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!c.vg) { teardown(c); return false; }
    c.gui = p5ui::create(c.vg);
    c.hasGui = c.gui.fontId >= 0;
    return c.hasGui;
}

// ---- clap_plugin_gui surface --------------------------------------------------

bool gui_is_api_supported(const clap_plugin_t *, const char *api, bool is_floating)
{
    return !is_floating && api && !std::strcmp(api, CLAP_WINDOW_API_X11);
}
bool gui_get_preferred_api(const clap_plugin_t *, const char **api, bool *is_floating)
{
    *api = CLAP_WINDOW_API_X11; *is_floating = false; return true;
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
        c.copyText = x11CopyText;
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
    if (auto *c = ctxFrom(p)) { c->scale = scale; return true; }
    return false;
}
bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
    double scale = 1.0;
    if (auto *c = ctxFrom(p)) if (c->scale > 0.0) scale = c->scale;
    *w = (uint32_t)(p5ui::kPanelW * 0.75 * scale);
    *h = (uint32_t)(p5ui::kPanelH * 0.75 * scale);
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
    // The host's size is authoritative: resize EXACTLY to it. (Forcing our own
    // aspect here left a host-painted band under the window — that was the
    // "tall window". Aspect belongs to the drag hints, not to set_size.)
    if (w == 0 || h == 0) return false;
    c->reqW = w; c->reqH = h;
    if (c->win)
    {
        XResizeWindow(c->dpy, c->win, (int)w, (int)h);
        c->w = w; c->h = h;
        render(*c);
    }
    return true;
}
bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *window)
{
    std::fprintf(stderr, "[p5gui] set_parent api=%s\n", window ? window->api : "(null)");
    auto *c = ctxFrom(p);
    if (!c || !window || std::strcmp(window->api, CLAP_WINDOW_API_X11) != 0) return false;
    return setupEmbedded(*c, (Window)window->x11);
}
bool gui_set_transient(const clap_plugin_t *, const clap_window_t *) { return false; }
void gui_suggest_title(const clap_plugin_t *, const char *) {}
bool gui_show(const clap_plugin_t *p)
{
    std::fprintf(stderr, "[p5gui] show\n");
    auto *c = ctxFrom(p);
    if (!c || !c->win) return false;
    XMapWindow(c->dpy, c->win);
    c->mapped = true;
    registerTimer(*c);
    render(*c);
    return true;
}
bool gui_hide(const clap_plugin_t *p)
{
    auto *c = ctxFrom(p);
    if (!c || !c->win) return false;
    unregisterTimer(*c);
    XUnmapWindow(c->dpy, c->win);
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

// timer-support: pump X events + repaint
static void gui_on_timer(const clap_plugin_t *p, clap_id)
{
    if (auto *c = ctxFrom(p)) { pump(*c); }
}
const clap_plugin_timer_support_t kTimerSupport = { gui_on_timer };

} // namespace pw
