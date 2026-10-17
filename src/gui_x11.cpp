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
#include <unistd.h>
#include <ctime>

#include "gui_core.hpp"
#include "p5_store.hpp"
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
    bool floating = false;
    bool keepVisible = false;                 // float: host show/hide must not unmap us
    bool wantFloat = false;                   // deferred DETACH (see pump tail)                    // clap gui floating mode: we own the window
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

static const char *gModeSource = "default";
static bool gLearnedFloat = false;   // one-shot per process
static bool gExplicitEmbed = false;  // gui.conf said mode=embed: beats policy
static uint32_t gFloatW = 0, gFloatH = 0;    // remembered FLOAT-window size (0 = unset)

// gui.conf is the plugin's own territory; float mode also stores the
// window's CURRENT size so a hand-made window comes back the same tomorrow.
static void writeGuiConfFloat(uint32_t w, uint32_t h)
{   const std::string dir = p5store::baseDir();
    if (dir.empty()) return;
    if (FILE *f = std::fopen((dir + "/gui.conf").c_str(), "wb"))
    {   std::fprintf(f, "# Muhammad GUI window mode\nmode=float\nw=%u\nh=%u\n", w, h);
        std::fclose(f);
    }
}
static bool modeFloat()
{
    // Persistent user control: <store>/gui.conf containing a line
    //   mode=float   (or mode=embed).   The store dir is the plugin's own
    // territory (Documents/Muhammad Synth) - no environment has to survive
    // Bitwig's plugin-host process fork, and the file is something the
    // plugin can also WRITE for itself (see autoLearnFloat).
    // Env P5_GUI_MODE stays as the developer override; explicit config wins.
    std::string conf;
    {   const std::string dir = p5store::baseDir();
        if (!dir.empty()) conf = dir + "/gui.conf";   }
    FILE *f = conf.empty() ? nullptr : std::fopen(conf.c_str(), "rb");
    if (f)
    {   char ln[128]; bool fl = false;
        while (std::fgets(ln, sizeof ln, f))
        {   if (!std::strncmp(ln, "mode=float", 10)) fl = true;
            else if (!std::strncmp(ln, "w=", 2)) gFloatW = (uint32_t)std::strtoul(ln + 2, nullptr, 10);
            else if (!std::strncmp(ln, "h=", 2)) gFloatH = (uint32_t)std::strtoul(ln + 2, nullptr, 10);
        }
        std::fclose(f);
        if (gFloatW < 400 || gFloatW > 4096) gFloatW = 0;   // sanity: a real panel
        if (gFloatH < 110 || gFloatH > 1536) gFloatH = 0;   // width, any era
        if (!fl) gExplicitEmbed = true;
        gModeSource = fl ? "config" : "config(embed)";
        return fl;
    }
    const char *m = std::getenv("P5_GUI_MODE");
    if (m && !std::strcmp(m, "float")) { gModeSource = "env"; return true; }
    if (m && !std::strcmp(m, "embed")) { gModeSource = "env(embed)"; return false; }
    gModeSource = "default";
    return false;
}
static void autoLearnFloat()
{
    // The host just PROVED it owns the embedded slot (never asked our size,
    // answered every ask, shrunk nothing). Record that verdict persistently
    // so the NEXT session floats with zero user action; log it loudly.
    const std::string dir = p5store::baseDir();
    if (dir.empty()) return;
    const std::string conf = dir + "/gui.conf";
    if (FILE *f = std::fopen(conf.c_str(), "wb"))
    {   std::fprintf(f, "# Muhammad GUI window mode\nmode=float\n");
        std::fclose(f);
        std::fprintf(stderr, "[p5gui] host proved it owns the embedded slot; wrote %s -> next session floats\n", conf.c_str());
    }
}

void switchToFloat(Ctx &c);

void pump(Ctx &c)
{
    bool dirty = false;
    if (!c.floating && c.parent && c.win)
    {
        XWindowAttributes pa{};
        if (XGetWindowAttributes(c.dpy, c.parent, &pa))
        {
            const long t = nowMs();
            uint32_t ow, oh;
            if (coreMirrorSlot(c, pa.width, pa.height, ow, oh, t))
            {   XResizeWindow(c.dpy, c.win, ow, oh);
                dirty = true; }
            // Hosts that hand out a square slot and refuse to shrink it
            // (Bitwig: ignores our aspect hints; its drag keeps the launch
            // aspect forever) still react to WM-side resizes of their OWN
            // top-level window - so when the host GUI service failed us,
            // trim the ancestor's excess ourselves. Budget 2, re-armed by
            // any configure.
            const long wantH = (long)(pa.width * p5ui::kPanelH / p5ui::kPanelW);
            // LEARN when the slot is proven hostile by EITHER exhaustion
            // path: the trim budget spent (fighting an always-re-asserting
            // host, my harness) OR the anti-fight stop reached (Bitwig's
            // trims came back larger -> ineffective -> stopped at 2 of 10,
            // which the first learn gate waited on and could thus never
            // fire - the user had to set gui.conf by hand). get_size never
            // called = the host doesn't consult the plugin's size at all,
            // the decisive tell; cooperative hosts (Ardour's wrapper) have
            // a non-zero counter and are never learned against.
            const bool trimmedOut = c.topTries >= 10 || c.trimIneffective >= 2;
            if (trimmedOut && pa.height > wantH + 40 && !c.getSizeCalls &&
                !gLearnedFloat)
            {   gLearnedFloat = true; autoLearnFloat(); switchToFloat(c); }
            if (pa.height > wantH + 40 && c.topTries < 10 &&
                c.trimIneffective < 2 && coreShouldTrimNow(c, t))
            {
                if (t >= c.topNextMs)
                {
                    c.topNextMs = t + 1500;
                    Window top = c.parent, r2, up, *k = nullptr; unsigned n2 = 0;
                    for (int guard = 0; guard < 8; guard++)
                    {
                        if (!XQueryTree(c.dpy, top, &r2, &up, &k, &n2)) break;
                        if (k) XFree(k);
                        if (up == 0 || up == top || up == r2) break;   // r2 == root
                        top = up;
                    }
                    XWindowAttributes ta{};
                    if (XGetWindowAttributes(c.dpy, top, &ta) && ta.width > 1)
                    {   c.topTries++;
                        if (c.hostName[0] == 'u')          // first visit: grab identity too
                        {   XClassHint ch{};
                            if (XGetClassHint(c.dpy, top, &ch) && ch.res_class)
                            {   std::snprintf(c.hostName, sizeof c.hostName, "%.64s", ch.res_class);
                                XFree(ch.res_class); }
                            if (ch.res_name) XFree(ch.res_name); }
                        const int nh = ta.height - (pa.height - (int)wantH);
                        const int tgtH = nh > (int)wantH ? nh : (int)wantH;
                        std::fprintf(stderr, "[p5gui] host kept slot %dx%d; shrinking top 0x%lx %dx%d -> %dx%d + holder 0x%lx (try %d)\n",
                                     pa.width, pa.height, (unsigned long)top,
                                     ta.width, ta.height, ta.width, tgtH,
                                     (unsigned long)c.parent, c.topTries);
                        // Resize the WM top-level AND the immediate holder: JUCE
                        // re-lays out from whichever component actually shrinks.
                        XResizeWindow(c.dpy, top, ta.width, tgtH);
                        if (top != c.parent) XResizeWindow(c.dpy, c.parent, pa.width, (int)wantH);
                        XFlush(c.dpy);
                        XSync(c.dpy, False);                 // apply, then read truth
                        XWindowAttributes va{};
                        if (XGetWindowAttributes(c.dpy, top, &va))
                        {   c.trimWid = (unsigned long)top;
                            c.trimAfterW = (uint32_t)va.width; c.trimAfterH = (uint32_t)va.height;
                            // Bitwig measured: the window came back LARGER (1308).
                            // Two consecutive ineffective trims stop the fight -
                            // an embedded host owns its slot; float mode is the answer.
                            if (c.trimAfterH + 8 >= (uint32_t)ta.height) c.trimIneffective++;
                            else c.trimIneffective = 0; }
                        std::fprintf(stderr, "[p5gui] trim readback: top 0x%lx now %dx%d\n",
                                     (unsigned long)top, va.width, va.height);
                    }
                }
            }
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
                // pp == root is NOT a wrapper to grow into: asking the host for
                // the screen's height was noise (and the wrong direction now
                // that we trim). Only a real wrapper ancestor counts.
                if (XQueryTree(c.dpy, c.parent, &r, &pp, &kids, &nk) && pp && pp != r)
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
                    if (c.floating)          // the hand-made window is the user's
                    {   static long gLastConfWrite = 0;   // answer; remember it
                        const long t = nowMs();           // (throttled: a drag is
                        if (t - gLastConfWrite > 900)     // a storm of configures)
                        {   gLastConfWrite = t; writeGuiConfFloat(c.w, c.h); }
                    }
                    // Landed at (or near) the panel aspect - the goal state.
                    // Re-arm only the TRIM side; the ask cap is lifetime (a
                    // host may only touch aspect transiently during setup and
                    // wiping totalAsks here produced the dead "asks 0/3
                    // ACCEPTED" state in Bitwig: stale throttle, no re-ask,
                    // no give-up, no trim).
                    const long wantH2 = (long)ev.xconfigure.width * p5ui::kPanelH / p5ui::kPanelW;
                    if (labs((long)ev.xconfigure.height - wantH2) <= 40)
                    {   c.topTries = 0; c.askNextMs = 0; c.giveUpOnHost = false; }
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
                {   if (onRightPress(c, (float)ev.xbutton.x, (float)ev.xbutton.y))
                        dirty = true;          // the flip itself repaints the dial
                    else
                        dirty |= onPointerPress(c, (float)ev.xbutton.x, (float)ev.xbutton.y,
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
                else if (c.floating && ev.xclient.window == c.win && c.wmDelete != None)
                {   const Atom wmProtos = XInternAtom(c.dpy, "WM_PROTOCOLS", True);
                    if (ev.xclient.message_type == wmProtos &&
                        (Atom)ev.xclient.data.l[0] == c.wmDelete)
                    {   std::fprintf(stderr, "[p5gui] floating window closed by user\n");
                        if (c.hostGui && c.hostGui->closed && c.plugin && c.plugin->host)
                        {   c.keepVisible = false;   // user killed it: nothing to guard
                            c.hostGui->closed(c.plugin->host, false);   // host stays in sync
                        }
                        unregisterTimer(c);
                        XUnmapWindow(c.dpy, c.win);
                        c.mapped = false;
                    }
                }
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
    if (c.wantFloat && !c.floating)
    {   c.wantFloat = false;
        gLearnedFloat = true;      // user chose; suppress redundant auto-learn
        autoLearnFloat();          // persist: next session floats from tick one
        switchToFloat(c);          // AND escape this session right now
    }
    if (c.plugin) c.plugin->maybePingLatency();
    // (shrink-negotiation removed: surplus height is now DRAWN chassis, so
    // asking the host to shrink it would fight the ancestor-grow request)
}

// ---- X protocol error defense ------------------------------------------------
// A host that reclaims our windows (device delete, layout teardown) can leave
// every LATER X call of ours aimed at dead XIDs. Xlib's DEFAULT error handler
// calls exit() - which is precisely "Bitwig's audio engine crashes when we
// delete the instance". We install a handler that IGNORES errors on OUR
// windows only, chaining everything else to whatever the host had installed,
// and restore theirs at gui_destroy.
static Ctx *gActiveCtx = nullptr;
static int (*gPrevXErr)(Display *, XErrorEvent *) = nullptr;
static int x11ErrorHandler(Display *d, XErrorEvent *e)
{
    Ctx *c = gActiveCtx;
    if (c && (e->resourceid == (unsigned long)c->win ||
              e->resourceid == (unsigned long)c->parent))
    {   char b[96]; XGetErrorText(d, e->error_code, b, sizeof b);
        std::fprintf(stderr, "[p5gui] X11 error on our window ignored: %s\n", b);
        return 0; }
    return gPrevXErr ? gPrevXErr(d, e) : 0;
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

bool setupFloating(Ctx &c)
{
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
    swa.colormap = XCreateColormap(c.dpy, DefaultRootWindow(c.dpy), vi->visual, AllocNone);
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask | KeyPressMask;
    // ASK SMALL: the window opens at the device-view width (~1052) at exact
    // panel aspect - not the oversized 0.75-scale slab. Whatever the user
    // drags it to is remembered (gui.conf) and asked for next time.
    const uint32_t w = gFloatW ? gFloatW
                   : (uint32_t)(1052.0 * (c.scale > 0.0 ? c.scale : 1.0));
    const uint32_t h = (uint32_t)(w * p5ui::kPanelH / p5ui::kPanelW);
    c.w = w; c.h = h;
    c.win = XCreateWindow(c.dpy, DefaultRootWindow(c.dpy), 0, 0, (int)w, (int)h, 0,
                          vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XFree(vi);
    if (!c.win) { teardown(c); return false; }
    clip::win = c.win;

    XStoreName(c.dpy, c.win, "Muhammad");
    XClassHint ch{};
    ch.res_name = const_cast<char *>("muhammad");
    ch.res_class = const_cast<char *>("Muhammad");
    XSetClassHint(c.dpy, c.win, &ch);
    c.wmDelete = XInternAtom(c.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(c.dpy, c.win, &c.wmDelete, 1);
    if (c.parent) XSetTransientForHint(c.dpy, c.win, c.parent);
    XSizeHints *sh = XAllocSizeHints();
    sh->flags = PAspect | PMinSize | PSize;
    sh->width = (int)w; sh->height = (int)h;
    sh->min_aspect.x = sh->max_aspect.x = (int)p5ui::kPanelW;   // drags stay panel-true
    sh->min_aspect.y = sh->max_aspect.y = (int)p5ui::kPanelH;
    sh->min_width = 520; sh->min_height = 139;
    XSetWMNormalHints(c.dpy, c.win, sh);
    XFree(sh);
    // NanoVG must be initialised against THIS window (the embedded path does
    // it at the end of setupEmbedded): render() early-outs on !c.vg, so
    // without this the floating window mapped white-on-white.
    if (!glXMakeCurrent(c.dpy, c.win, c.gl)) { teardown(c); return false; }
    c.vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!c.vg) { teardown(c); return false; }
    c.gui = p5ui::create(c.vg);
    c.hasGui = c.gui.fontId >= 0;
    std::fprintf(stderr, "[p5gui] FLOATING window 0x%lx %ux%u, panel aspect WM-locked, vg %s\n",
                 (unsigned long)c.win, c.w, c.h, c.hasGui ? "ok" : "FONT FAIL");
    return c.hasGui;
}

void switchToFloat(Ctx &c)
{
    // Mid-session escape from a host that just proved it owns the embedded
    // slot: tear our child window down and rebuild as our OWN top-level.
    // The host timer stays registered (same Ctx, same pump); clip:: rebinds
    // inside setupFloating. The host keeps thinking it embedded us - it is
    // wrong, visibly, and that is the point.
    std::fprintf(stderr, "[p5gui] switching to FLOATING mid-session\n");
    if (c.plugin) c.plugin->trail("switch_to_float");
    const Window anchor = c.parent;
    const uint32_t rw = c.w, rh = c.h;
    teardown(c);
    c.parent = anchor;
    c.floating = true;
    c.reqW = rw; c.reqH = rh;
    if (setupFloating(c))
    {   XMapWindow(c.dpy, c.win);
        XMapRaised(c.dpy, c.win);
        c.mapped = true;
        c.keepVisible = true;   // the host does NOT own this window anymore
        render(c);
        // Bitwig otherwise keeps the wrapper window open - empty, black -
        // until the track is deleted (user-measured). Ask the host to hide
        // the GUI slot it no longer hosts; if it refuses, announce the
        // connection as lost per spec (was_destroyed=true: its follow-up
        // gui_destroy is survived - the float window is ours).
        const clap_host_t *hh = c.plugin ? c.plugin->host : nullptr;
        if (c.hostGui && c.hostGui->request_hide && hh && c.hostGui->request_hide(hh))
            std::fprintf(stderr, "[p5gui] asked host to hide the empty wrapper\n");
        else if (c.hostGui && c.hostGui->closed && hh)
        {   // was_destroyed=false: "connection lost, GUI still alive" - do NOT
            // invite a gui_destroy (its teardown would kill the live float).
            std::fprintf(stderr, "[p5gui] request_hide refused -> closed(host,false)\n");
            c.hostGui->closed(hh, false);
        }
    }
    else std::fprintf(stderr, "[p5gui] mid-session float setup FAILED (blank slot)\n");
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
    {   Window top = c.parent, r2, up, *k = nullptr; unsigned n2 = 0;
        for (int guard = 0; guard < 8; guard++)
        {   if (!XQueryTree(c.dpy, top, &r2, &up, &k, &n2)) break;
            if (k) XFree(k);
            if (up == 0 || up == top || up == r2) break;
            top = up;
        }
        XClassHint ch{};
        if (XGetClassHint(c.dpy, top, &ch) && ch.res_class)
        {   std::snprintf(c.hostName, sizeof c.hostName, "%.64s", ch.res_class);
            XFree(ch.res_class); }
        if (ch.res_name) XFree(ch.res_name);
        std::fprintf(stderr, "[p5gui] host top-level: %s\n", c.hostName);
    }

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
    sh->flags = PAspect | PMinSize | PSize;
    sh->width = (int)(p5ui::kPanelW * 0.75); sh->height = (int)(p5ui::kPanelH * 0.75);
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
    (void)is_floating;   // BOTH embedded and floating X11 windows are supported
    return api && !std::strcmp(api, CLAP_WINDOW_API_X11);
}

bool gui_get_preferred_api(const clap_plugin_t *, const char **api, bool *is_floating)
{
    // Default embedded (polite citizen). P5_GUI_MODE=float declares a
    // PLUGIN-FLOATING preference: hosts like Bitwig 5.0.4 (measured: get_size
    // called 0 times, request_resize answered "ACCEPTED" but re-asserted,
    // programmatic shrinks undone by their own layout) own the embedded
    // slot's size utterly - inside such a slot the panel can only be SCALED
    // (chassis), never sized right. A floating window is OURS: exact panel
    // aspect locked via WM_NORMAL_HINTS, WM decorations, correct drag,
    // user-close -> hostGui->closed so the host stays in sync.
    *api = CLAP_WINDOW_API_X11; *is_floating = modeFloat(); return true;
}
bool gui_create(const clap_plugin_t *p, const char *api, bool is_floating)
{
    std::fprintf(stderr, "[p5gui] create api=%s floating=%d\n", api ? api : "(null)", (int)is_floating);
    if (!gui_is_api_supported(p, api, is_floating)) return false;
    auto *s = static_cast<Plugin *>(p->plugin_data);
    if (!s->gui)
    {
        s->gui = new Ctx(s);
        Ctx &c = *static_cast<Ctx *>(s->gui);
        c.copyText = x11CopyText;
        c.requestFloat = [](GuiCore &g) { static_cast<Ctx &>(g).wantFloat = true; };
        if (s->host && s->host->get_extension)
            c.hostGui = (const clap_host_gui_t *)s->host->get_extension(s->host, CLAP_EXT_GUI);
        std::fprintf(stderr, "[p5gui] host clap.gui service %s\n", c.hostGui ? "present" : "absent");
    }
    if (auto *s0 = static_cast<Plugin *>(p->plugin_data))
    {   char b[64]; std::snprintf(b, sizeof b, "gui_create api=%s float=%d", api?api:"-", (int)is_floating);
        s0->trail(b); }
    if (auto *cc = ctxFrom(p))
    {   if (api) std::strncpy(cc->guiApi, api, sizeof cc->guiApi - 1);
        cc->floating = is_floating || modeFloat();  // env can FORCE float
        cc->modeSource = cc->floating ? gModeSource : "embedded";
            cc->modeSource = cc->floating ? gModeSource : "embedded";
        gActiveCtx = static_cast<Ctx *>(cc);
        if (!gPrevXErr) gPrevXErr = XSetErrorHandler(x11ErrorHandler);
        std::fprintf(stderr, "[p5gui] create mode: %s (source: %s)\n",
                     cc->floating ? (is_floating ? "FLOATING" : "FLOATING (forced)")
                                  : "embedded", gModeSource); }
    return true;
}
void gui_destroy(const clap_plugin_t *p)
{
    auto *s = static_cast<Plugin *>(p->plugin_data);
    s->trail("gui_destroy");
    if (!s->gui) return;
    Ctx &c = *static_cast<Ctx *>(s->gui);
    unregisterTimer(c);
    teardown(c);
    delete &c;
    s->gui = nullptr;
    if (gActiveCtx == &c)
    {   gActiveCtx = nullptr;
        XSetErrorHandler(gPrevXErr); gPrevXErr = nullptr; }
}
bool gui_set_scale(const clap_plugin_t *p, double scale)
{
    std::fprintf(stderr, "[p5gui] set_scale %g\n", scale);
    if (auto *c = ctxFrom(p)) { c->scale = scale; return true; }
    return false;
}
bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
    if (auto *cc = ctxFrom(p)) cc->getSizeCalls++;
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
    if (c->floating)
    {   // P5_GUI_MODE=float with a host that still requested embedded (Bitwig
        // may ignore get_preferred_api): accept politely, remember their window
        // as the transient anchor, and let show() create OUR OWN top-level.
        c->parent = (Window)window->x11;
        std::fprintf(stderr, "[p5gui] set_parent absorbed: FLOATING forced\n");
        return true;
    }
    return setupEmbedded(*c, (Window)window->x11);
}
bool gui_set_transient(const clap_plugin_t *p, const clap_window_t *window)
{
    auto *c = ctxFrom(p);
    std::fprintf(stderr, "[p5gui] set_transient api=%s\n", window ? window->api : "(null)");
    if (!c || !window || std::strcmp(window->api, CLAP_WINDOW_API_X11) != 0) return false;
    c->parent = (Window)window->x11;      // grouping anchor; float setup applies the hint
    if (c->win) XSetTransientForHint(c->dpy, c->win, c->parent);
    return true;
}
void gui_suggest_title(const clap_plugin_t *, const char *) {}
bool gui_show(const clap_plugin_t *p)
{
    std::fprintf(stderr, "[p5gui] show\n");
    if (auto *s0 = static_cast<Plugin *>(p->plugin_data)) s0->trail("gui_show");
    auto *c = ctxFrom(p);
    if (!c) return false;
    if (c->floating && !c->win && !setupFloating(*c)) return false;
    if (!c->win) return false;
    XMapWindow(c->dpy, c->win);
    XMapRaised(c->dpy, c->win);
    c->mapped = true;
    registerTimer(*c);
    render(*c);
    return true;
}
bool gui_hide(const clap_plugin_t *p)
{
    if (auto *s0 = static_cast<Plugin *>(p->plugin_data)) s0->trail("gui_hide");
    auto *c = ctxFrom(p);
    if (!c || !c->win) return false;
    if (!c->mapped && c->plugin) { c->plugin->trail("gui_hide: already hidden"); return true; }
    if (c->keepVisible)
    {   std::fprintf(stderr, "[p5gui] host hide ignored: window is DETACHED\n");
        return true;                          // float window stays on screen
    }
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
