// SPDX-License-Identifier: GPL-3.0-only
// clap-gui backend: an X11 child window embedded in the host's window, GLX +
// NanoVG GL3 rendering the shared p5ui panel, mouse gestures back into CLAP
// params (via Plugin::guiSet, drained to the host on the next params.flush).
// Linux is the primary target; win32/cocoa tables can follow the same shape.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <GL/glx.h>
#include <GL/gl.h>

#include <nanovg.h>
#define NANOVG_GL3 1
#include <nanovg_gl.h>

#include <clap/clap.h>
#include <clap/ext/timer-support.h>

#include <cstdio>
#include <cstring>

#include "prophet_plugin.hpp"
#include "p5_common.hpp"
#include "p5_gui.hpp"

namespace pw {

namespace {

struct Ctx
{
    explicit Ctx(Plugin *p) : plugin(p) {}

    Plugin *plugin;
    Display *dpy = nullptr;
    Window parent = 0, win = 0;
    Atom xembed = None, xembedInfo = None;
    GLXContext gl = nullptr;
    Atom wmDelete = None;
    NVGcontext *vg = nullptr;
    p5ui::Gui gui{};
    bool hasGui = false;

    double scale = 1.0;                 // host set_scale (physical px per layout unit)
    uint32_t w = 0, h = 0;              // current pixel size
    bool mapped = false;

    // drag state
    int dragKnob = -1, pressBtn = -1;
    int dragStartY = 0;
    double dragStartNorm = 0.0;
    bool pointerGrabbed = false;

    clap_id timerId = 0;
    bool timerRegistered = false;

    uint32_t reqW = 0, reqH = 0;   // set_size may arrive before set_parent
};

Ctx *ctxFrom(const clap_plugin_t *p)
{
    auto *s = static_cast<Plugin *>(p->plugin_data);
    return s ? static_cast<Ctx *>(s->gui) : nullptr;
}

// ---- state <-> layout -------------------------------------------------------

void fillState(Ctx &c, p5ui::State &st)
{
    for (int i = 0; i < p5ui::kNumKnobs; i++)
        st.knob[i] = p5::knobNorm(p5ui::kKnobs[i].pid, c.plugin->guiValue(p5ui::kKnobs[i].pid));
    const int program = c.plugin->program, bank = c.plugin->bank;
    for (int i = 0; i < p5ui::kNumBtns; i++)
    {
        const auto &b = p5ui::kBtns[i];
        if (b.ledmode == 1) st.led[i] = c.plugin->guiValue(b.pid) != 0.0;
        else if (b.ledmode == 2) st.led[i] = program >= 0 && program == bank * 8 + b.value;
    }
#ifdef P5_BUILD_INFO
    st.build = P5_BUILD_INFO;
#endif
    std::memcpy(st.name, c.plugin->engine.patch().raw + p5::P5_NAME, 20);
    st.name[20] = 0;
    for (char *q = st.name + 19; q >= st.name && *q == ' '; *q-- = 0) {}
    st.bank = program >= 0 ? program / 8 + 1 : 0;
    st.program = program >= 0 ? program % 8 + 1 : 0;
    // A bank cursor that has moved away from the loaded patch's bank shows in
    // parentheses, so stepping banks never looks like the patch moved.
    const int pb = c.plugin->bank + 1;
    if (program >= 0)
    {
        if (pb != st.bank) std::snprintf(st.bp, sizeof st.bp, "%d-%d (%d)", st.bank, st.program, pb);
        else std::snprintf(st.bp, sizeof st.bp, "%d-%d", st.bank, st.program);
    }
    else if (c.plugin->bank != 0) std::snprintf(st.bp, sizeof st.bp, "(%d)", pb);
    else st.bp[0] = 0;
}

// param value from a normalized knob position, in the param's own units
double normToValue(int pid, double norm)
{
    const p5::KnobRange *r = p5::knobRange(pid);
    double lo = r ? r->lo : 0.0, hi = r ? r->hi : (pid < 55 ? 127.0 : 255.0);
    return lo + norm * (hi - lo);
}

// ---- draw / pump -------------------------------------------------------------

void render(Ctx &c)
{
    if (!c.vg || !c.mapped) return;
    glXMakeCurrent(c.dpy, c.win, c.gl);
    glViewport(0, 0, (int)c.w, (int)c.h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    p5ui::State st;
    fillState(c, st);
    nvgBeginFrame(c.vg, (float)c.w, (float)c.h, 1.0f);
    p5ui::draw(c.gui, st, (float)c.w, (float)c.h);
    nvgEndFrame(c.vg);
    glXSwapBuffers(c.dpy, c.win);
}

void actuate(Ctx &c, int btnIdx)
{
    const auto &b = p5ui::kBtns[btnIdx];
    if (b.pid < 0) return;                            // decorative
    if (b.ledmode == 2)                               // program select
    {
        const int k = c.plugin->bank * 8 + b.value;
        if (k < (int)PW_FACTORY_N) c.plugin->guiSet(PID_PROGRAM, k);
        return;
    }
    if (b.pid == PID_BANK)
    {
        const int banks = (int)PW_FACTORY_N / 8;
        c.plugin->guiSet(PID_BANK, (double)((c.plugin->bank + 1) % banks));
        return;
    }
    c.plugin->guiSet((clap_id)b.pid, c.plugin->guiValue((clap_id)b.pid) != 0.0 ? 0.0 : 1.0);
}

void pump(Ctx &c)
{
    bool dirty = false;
    if (c.parent && c.win)
    {
        XWindowAttributes pa{};
        if (XGetWindowAttributes(c.dpy, c.parent, &pa) && pa.width > 1 && pa.height > 1 &&
            ((uint32_t)pa.width != c.w || (uint32_t)pa.height != c.h))
        {
            static int mirrorLogged = 0;
            if (!mirrorLogged++)
                std::fprintf(stderr, "[p5gui] mirroring parent geometry -> %dx%d\n", pa.width, pa.height);
            XResizeWindow(c.dpy, c.win, pa.width, pa.height);
            c.w = (uint32_t)pa.width; c.h = (uint32_t)pa.height;
            dirty = true;
        }
    }
    XEvent ev;
    while (XPending(c.dpy))
    {
        XNextEvent(c.dpy, &ev);
        if (ev.xany.window != c.win) continue;
        switch (ev.type)
        {
            case Expose:
                if (ev.xexpose.count == 0) render(c);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0 &&
                    ((uint32_t)ev.xconfigure.width != c.w || (uint32_t)ev.xconfigure.height != c.h))
                {
                    c.w = (uint32_t)ev.xconfigure.width;
                    c.h = (uint32_t)ev.xconfigure.height;
                    render(c);
                }
                break;
            case ButtonPress:
                if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
                {
                    const int k = p5ui::knobAt(p5ui::State{}, (float)ev.xbutton.x, (float)ev.xbutton.y,
                                               (float)c.w, (float)c.h);
                    if (k >= 0)
                    {
                        const int pid = p5ui::kKnobs[k].pid;
                        double v = c.plugin->guiValue(pid);
                        const p5::KnobRange *r = p5::knobRange(pid);
                        const double step = r ? (r->hi - r->lo) / 50.0 : 5.0;
                        v += (ev.xbutton.button == Button4 ? step : -step);
                        c.plugin->guiSet((clap_id)pid, normToValue(pid, p5::knobNorm(pid, v)));
                        dirty = true;
                    }
                    break;
                }
                if (ev.xbutton.button != Button1) break;
                c.dragKnob = p5ui::knobAt(p5ui::State{}, (float)ev.xbutton.x, (float)ev.xbutton.y,
                                          (float)c.w, (float)c.h);
                if (c.dragKnob >= 0)
                {
                    c.dragStartY = ev.xbutton.y;
                    c.dragStartNorm = p5::knobNorm(p5ui::kKnobs[c.dragKnob].pid,
                                                   c.plugin->guiValue(p5ui::kKnobs[c.dragKnob].pid));
                    c.pressBtn = -1;
                    XGrabPointer(c.dpy, c.win, True,
                                 ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync,
                                 None, None, CurrentTime);
                    c.pointerGrabbed = true;
                }
                else
                {
                    c.pressBtn = p5ui::btnAt(p5ui::State{}, (float)ev.xbutton.x, (float)ev.xbutton.y,
                                             (float)c.w, (float)c.h);
                }
                break;
            case ButtonRelease:
                if (ev.xbutton.button != Button1) break;
                if (c.pointerGrabbed) { XUngrabPointer(c.dpy, CurrentTime); c.pointerGrabbed = false; }
                if (c.pressBtn >= 0 &&
                    p5ui::btnAt(p5ui::State{}, (float)ev.xbutton.x, (float)ev.xbutton.y,
                                (float)c.w, (float)c.h) == c.pressBtn)
                { actuate(c, c.pressBtn); dirty = true; }   // LEDs must flip NOW, not on the next tick
                if (c.dragKnob >= 0)                        // gesture ends: tell the host the FINAL value
                { c.plugin->guiSync((clap_id)p5ui::kKnobs[c.dragKnob].pid); dirty = true; }
                c.dragKnob = -1; c.pressBtn = -1;
                break;
            case MotionNotify:
                if (c.dragKnob >= 0)
                {
                    const double dy = (double)(c.dragStartY - ev.xmotion.y);
                    double norm = c.dragStartNorm + dy * p5ui::kDragRangePer100px / 100.0;
                    if (ev.xmotion.state & ShiftMask)   // fine: rebase from start
                        norm = c.dragStartNorm + dy * p5ui::kDragRangePer100px / 100.0 * 0.2;
                    if (norm < 0) norm = 0;
                    if (norm > 1) norm = 1;
                    const int pid = p5ui::kKnobs[c.dragKnob].pid;
                    c.plugin->guiSetLocal((clap_id)pid, normToValue(pid, norm));
                    render(c);
                }
                break;
            case ClientMessage:
                if (ev.xclient.message_type == c.xembed) { /* EMBEDDED/FOCUS/CONFIGURE lifecycle */ }
                break;
            default:
                break;
        }
    }
    if (dirty) render(c);
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
    c.dpy = XOpenDisplay(nullptr);
    if (!c.dpy) return false;
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
                     StructureNotifyMask;   // ClientMessage needs no mask
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
    XStoreName(c.dpy, c.win, "Prophet-5");
    std::fprintf(stderr, "[p5gui] child window created: 0x%lx parent 0x%lx size %ux%u\n",
                 (unsigned long)c.win, (unsigned long)c.parent, c.w, c.h);
    c.wmDelete = XInternAtom(c.dpy, "WM_DELETE_WINDOW", False);

    // Announce XEmbed (the clap spec's x11 embedding protocol) so the host
    // manages us as an embedded window, and pin the aspect in WM hints.
    c.xembed = XInternAtom(c.dpy, "_XEMBED", False);
    c.xembedInfo = XInternAtom(c.dpy, "_XEMBED_INFO", False);
    const long xembedInfoVal[2] = { 0 /*version*/, 1 /*XEMBED_MAPPED*/ };
    XChangeProperty(c.dpy, c.win, c.xembedInfo, XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)xembedInfoVal, 2);
    XClassHint classHint{};
    classHint.res_name = const_cast<char *>("prophet");
    classHint.res_class = const_cast<char *>("Prophet-5");
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

void registerTimer(Ctx &c)
{
    auto *host = c.plugin->host;
    if (!host || !host->get_extension) return;
    auto *ts = (const clap_host_timer_support_t *)host->get_extension(host, CLAP_EXT_TIMER_SUPPORT);
    if (ts && ts->register_timer && ts->register_timer(host, 16, &c.timerId))
        c.timerRegistered = true;
    std::fprintf(stderr, "[p5gui] timer-support %s\n", c.timerRegistered ? "registered" : "absent");
}

void unregisterTimer(Ctx &c)
{
    auto *host = c.plugin->host;
    if (!c.timerRegistered || !host || !host->get_extension) return;
    auto *ts = (const clap_host_timer_support_t *)host->get_extension(host, CLAP_EXT_TIMER_SUPPORT);
    if (ts && ts->unregister_timer) ts->unregister_timer(host, c.timerId);
    c.timerRegistered = false;
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
    auto *s = static_cast<Plugin *>(p->plugin_data);
    if (!s->gui) s->gui = new Ctx(s);
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

const clap_plugin_gui_t kGuiX11 = {
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
