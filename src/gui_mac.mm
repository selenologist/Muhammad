// SPDX-License-Identifier: GPL-3.0-only
// clap-gui backend (cocoa): an NSView embedded in the host's view, NSOpenGL
// core-profile context + NanoVG GL3. Same neutral core (gui_core.*) as the
// X11 and Win32 shims: this file owns the view, the GL context and the
// NSEvent -> on* translation. The CLAP cocoa gui API uses LOGICAL points for
// create/get/set size; our render space is physical px (logical * backing
// scale), matching the X11 convention the core expects for drag sensitivity.
//
// MRC (no ARC): we alloc/init the view and release it in teardown.
#include <cstdio>
#include <cstring>
#include <string>

#include <Cocoa/Cocoa.h>
#include <OpenGL/gl.h>

#include <nanovg.h>

#include "gui_core.hpp"
#include "muhammad_plugin.hpp"
#include "p5_gui.hpp"

namespace pw { struct Ctx; }            // fwd (an @interface cannot live in a namespace)

@interface P5PanelView : NSView
{
    @public
    pw::Ctx *core_;                      // set on attach; nil-safe everywhere
}
- (void)mouseMovedLike:(NSEvent *)e;
@end

namespace pw {

static double backingScale(NSView *v)
{
    NSWindow *w = v.window;
    const double s = w ? w.backingScaleFactor : 1.0;
    return s > 0.0 ? s : 1.0;
}

// Event point -> our physical-px window space (view is flipped: top-left
// origin, like X11/Win32 window pixels).
static void eventPx(NSView *v, NSEvent *e, float &px, float &py)
{
    const NSPoint p = [v convertPoint:e.locationInWindow fromView:nil];
    const double s = backingScale(v);
    px = (float)(p.x * s);
    py = (float)(p.y * s);
}

static unsigned eventMods(NSEvent *e)
{
    return (e.modifierFlags & NSEventModifierFlagShift) ? P5_MOD_SHIFT : 0u;
}

// ---------------------------------------------------------------------------
// platform context
// ---------------------------------------------------------------------------

struct Ctx : GuiCore
{
    explicit Ctx(Plugin *p) : GuiCore(p) {}

    NSView *parent = nil;               // borrowed (host-owned)
    P5PanelView *view = nil;            // we retain (alloc), release in teardown
    NSOpenGLContext *glctx = nil;       // we retain
    NVGcontext *vg = nullptr;
    p5ui::Gui gui{};
    bool hasGui = false;
    unsigned lastAskMs = 0;

    double s() const { return view ? backingScale(view) : 1.0; }

    void teardown()
    {
        if (vg)
        {
            if (glctx) [glctx makeCurrentContext];
            nvgDeleteGL3(vg);
            vg = nullptr;
            hasGui = false;
            [NSOpenGLContext clearCurrentContext];
        }
        if (glctx) { [glctx setView:nil]; [glctx release]; glctx = nil; }
        if (view)  { [view removeFromSuperview]; [view release]; view = nil; }
        parent = nil;
        mapped = false;
    }

    void requestRedraw() { if (view) [view setNeedsDisplay:YES]; }

    void consumeFocus()
    {
        if (needFocus && view.window)
        {   [view.window makeFirstResponder:view];
            needFocus = false;
        }
    }

    bool setupGL()
    {
        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFAAccelerated,
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAColorSize, 24,
            NSOpenGLPFAAlphaSize, 8,
            NSOpenGLPFAStencilSize, 8,
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
            0
        };
        NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!pf) { std::fprintf(stderr, "[p5gui] no 3.2-core pixel format\n"); return false; }
        glctx = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:nil];
        [pf release];
        if (!glctx) return false;
        [glctx setView:view];
        [glctx update];
        [glctx makeCurrentContext];
        const GLint swap = 1;                        // vsync with the display
        [glctx setValues:&swap forParameter:NSOpenGLCPSwapInterval];
        std::fprintf(stderr, "[p5gui] GL: %s\n", (const char *)glGetString(GL_VERSION));
        vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
        if (!vg) { [NSOpenGLContext clearCurrentContext]; return false; }
        gui = p5ui::create(vg);
        hasGui = gui.fontId >= 0;
        return hasGui;
    }

    bool attachTo(NSView *host)
    {
        parent = host;
        view = [[P5PanelView alloc] initWithFrame:host.bounds];
        view->core_ = this;
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        view.wantsLayer = YES;
        [host addSubview:view];
        std::fprintf(stderr, "[p5gui] NSView attached to host view, frame %.0fx%.0f\n",
                     host.bounds.size.width, host.bounds.size.height);
        if (!setupGL()) { teardown(); return false; }
        syncPixels();
        if (host.window) [host.window makeFirstResponder:view];
        return true;
    }

    // Our physical pixel size follows the view frame; the core works in
    // physical px (X11 convention).
    void syncPixels()
    {
        if (!view) return;
        const double sc = backingScale(view);
        const uint32_t nw = (uint32_t)(view.frame.size.width * sc + 0.5);
        const uint32_t nh = (uint32_t)(view.frame.size.height * sc + 0.5);
        if (nw && nh && (nw != w || nh != h))
        {   w = nw; h = nh;
            resizeRequests = 0;                      // host resized: renegotiate
            requestRedraw();
        }
    }

    // timer hook: throttled ancestor ask (mirrors the X11 grow logic) + focus
    void tick()
    {
        if (!view) return;
        syncPixels();
        consumeFocus();
        const long t = (long)([NSProcessInfo processInfo].systemUptime * 1000.0);
        if ((unsigned long)(t - (long)lastAskMs) >= 1000u)
        {
            lastAskMs = (unsigned)t;
            const double sc = s();
            NSView *par = view.superview;            // the host-provided view
            NSView *anc = par ? par.superview : nil;
            coreAncestorAsk(*this,
                            par ? (uint32_t)(par.bounds.size.width * sc + 0.5) : 0,
                            par ? (uint32_t)(par.bounds.size.height * sc + 0.5) : 0,
                            anc ? (uint32_t)(anc.bounds.size.width * sc + 0.5) : 0,
                            anc ? (uint32_t)(anc.bounds.size.height * sc + 0.5) : 0, t);
        }
    }

    void applyResize(uint32_t nw, uint32_t nh)      // logical points from host
    {
        const double sc = s();
        if (view) [view setFrame:NSMakeRect(0, 0, nw / sc, nh / sc)];
        syncPixels();
        requestRedraw();
    }
};

// ---------------------------------------------------------------------------
// the view: translate Cocoa events into the neutral core API
// ---------------------------------------------------------------------------

@implementation P5PanelView
{
    NSTrackingArea *tracking_;
}

- (instancetype)initWithFrame:(NSRect)f
{
    self = [super initWithFrame:f];
    if (self) tracking_ = nil;
    return self;
}

- (BOOL)isFlipped { return YES; }                   // top-left origin, like X11
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *) { return YES; }

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (tracking_) { [self removeTrackingArea:tracking_]; [tracking_ release]; tracking_ = nil; }
    tracking_ = [[NSTrackingArea alloc]
                 initWithRect:self.bounds
                      options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect
                        owner:self userInfo:nil];
    [self addTrackingArea:tracking_];
}

- (void)viewDidMoveToWindow
{
    if (core_ && self.window) [self.window makeFirstResponder:self];
}

- (void)drawRect:(NSRect)
{
    if (!core_ || !core_->vg) return;
    core_->syncPixels();
    [core_->glctx setView:self];
    [core_->glctx update];
    [core_->glctx makeCurrentContext];
    glViewport(0, 0, (GLsizei)core_->w, (GLsizei)core_->h);
    glClearColor(0.106f, 0.106f, 0.114f, 1.f);      // panel matte behind nvg
    glClear(GL_COLOR_BUFFER_BIT);
    core_->mapped = YES;
    renderFrame(*core_, core_->vg, core_->gui);
    [core_->glctx flushBuffer];
    [NSOpenGLContext clearCurrentContext];
    core_->consumeFocus();
}

- (void)viewDidLayout
{
    [super viewDidLayout];
    if (core_) core_->syncPixels();
}

- (void)mouseDown:(NSEvent *)e
{
    if (!core_) return;
    float px, py; eventPx(self, e, px, py);
    // double-click: NOT a native WM_LBUTTONDBLCLK equivalent here — the core's
    // 450 ms press-to-press detector sees the second press, same as X11.
    if (onPointerPress(*core_, px, py, eventMods(e), (unsigned)(e.timestamp * 1000.0)))
        [self setNeedsDisplay:YES];
    core_->consumeFocus();
}

- (void)mouseUp:(NSEvent *)e
{
    if (!core_) return;
    float px, py; eventPx(self, e, px, py);
    if (onPointerRelease(*core_, px, py, eventMods(e))) [self setNeedsDisplay:YES];
    core_->consumeFocus();
}

- (void)mouseDragged:(NSEvent *)e { [self mouseMovedLike:e]; }
- (void)mouseMoved:(NSEvent *)e   { [self mouseMovedLike:e]; }

- (void)mouseMovedLike:(NSEvent *)e
{
    if (!core_) return;
    float px, py; eventPx(self, e, px, py);
    if (onPointerMotion(*core_, px, py, eventMods(e))) [self setNeedsDisplay:YES];
}

- (void)rightMouseDown:(NSEvent *)e
{
    if (!core_) return;
    float px, py; eventPx(self, e, px, py);
    if (onPointerPress(*core_, px, py, P5_MOD_RIGHT, (unsigned)(e.timestamp * 1000.0)))
        [self setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent *)e
{
    if (!core_) return;
    float px, py; eventPx(self, e, px, py);
    const int dir = e.deltaY < 0 ? +1 : -1;          // down = next page, like Button5
    if (onWheel(*core_, px, py, dir)) [self setNeedsDisplay:YES];
}

- (void)keyDown:(NSEvent *)e
{
    if (!core_) { [super keyDown:e]; return; }
    GuiKey gk = KEY_NONE;
    switch (e.keyCode)
    {
        case 126: gk = K_UP; break;
        case 125: gk = K_DOWN; break;
        case 123: gk = K_LEFT; break;
        case 124: gk = K_RIGHT; break;
        case 36: case 76: gk = K_RETURN; break;
        case 53: gk = K_ESCAPE; break;
        case 51: gk = K_BACKSPACE; break;
        default: break;
    }
    bool dirty = false;
    if (gk != KEY_NONE) dirty = onKey(*core_, gk, eventMods(e));
    else
    {
        NSString *chars = e.characters;
        for (NSUInteger i = 0; i < chars.length && i < 8; i++)
        {
            const unichar ch = [chars characterAtIndex:i];
            if (ch >= 0x20 && ch < 0xE000 && ch != 0x7f)
                if (onChar(*core_, ch)) { dirty = true; break; }
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
    else [super keyDown:e];                          // cmd-combos, beeps pass through
    core_->consumeFocus();
}

- (void)dealloc
{
    if (tracking_) { [tracking_ release]; tracking_ = nil; }
    // Never touch core_ here: the Ctx may already be destroyed (teardown
    // released our last retain); everything needed is ivar-local.
    [super dealloc];
}
@end

// ---------------------------------------------------------------------------
// clap_plugin_gui surface (cocoa)
// ---------------------------------------------------------------------------

static Ctx *ctxFrom(const clap_plugin_t *p)
{
    auto *s = static_cast<Plugin *>(p->plugin_data);
    return s->gui ? static_cast<Ctx *>(s->gui) : nullptr;
}

static bool gui_is_api_supported(const clap_plugin_t *, const char *api, bool is_floating)
{
    return !is_floating && api && !std::strcmp(api, CLAP_WINDOW_API_COCOA);
}
static bool gui_get_preferred_api(const clap_plugin_t *, const char **api, bool *is_floating)
{
    *api = CLAP_WINDOW_API_COCOA; *is_floating = false; return true;
}
static bool gui_create(const clap_plugin_t *p, const char *api, bool is_floating)
{
    std::fprintf(stderr, "[p5gui] create api=%s floating=%d\n", api ? api : "(null)", (int)is_floating);
    if (!gui_is_api_supported(p, api, is_floating)) return false;
    auto *s = static_cast<Plugin *>(p->plugin_data);
    if (!s->gui)
    {
        s->gui = new Ctx(s);
        Ctx &c = *static_cast<Ctx *>(s->gui);
        if (s->host && s->host->get_extension)
            c.hostGui = (const clap_host_gui_t *)s->host->get_extension(s->host, CLAP_EXT_GUI);
        std::fprintf(stderr, "[p5gui] host clap.gui service %s\n", c.hostGui ? "present" : "absent");
    }
    return true;
}
static void gui_destroy(const clap_plugin_t *p)
{
    auto *s = static_cast<Plugin *>(p->plugin_data);
    if (!s->gui) return;
    Ctx &c = *static_cast<Ctx *>(s->gui);
    unregisterTimer(c);
    c.teardown();                                    // releases view + glctx
    delete &c;
    s->gui = nullptr;
}
static bool gui_set_scale(const clap_plugin_t *p, double scale)
{
    std::fprintf(stderr, "[p5gui] set_scale %g\n", scale);
    if (auto *c = ctxFrom(p)) { c->scale = scale; return true; }
    return false;
}
static bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
    // Cocoa contract: logical points. Natural proposal at 0.75 layout scale,
    // same as the other platforms; backing scaling is ours.
    (void)p;
    *w = (uint32_t)(p5ui::kPanelW * 0.75);
    *h = (uint32_t)(p5ui::kPanelH * 0.75);
    return true;
}
static bool gui_can_resize(const clap_plugin_t *) { return true; }
static bool gui_get_resize_hints(const clap_plugin_t *, clap_gui_resize_hints_t *hints)
{
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = true;
    hints->aspect_ratio_width = (uint32_t)p5ui::kPanelW;
    hints->aspect_ratio_height = (uint32_t)p5ui::kPanelH;
    return true;
}
static bool gui_adjust_size(const clap_plugin_t *, uint32_t *w, uint32_t *h)
{
    uint32_t nw = *w < 520u ? 520u : (*w > 3120u ? 3120u : *w);
    *w = nw; *h = (uint32_t)(nw * p5ui::kPanelH / p5ui::kPanelW);
    return true;
}
static bool gui_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h)
{
    std::fprintf(stderr, "[p5gui] set_size %ux%u\n", w, h);
    auto *c = ctxFrom(p);
    if (!c || !w || !h) return false;
    c->reqW = w; c->reqH = h;
    if (c->view) c->applyResize(w, h);               // logical points -> frame
    return true;
}
static bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *window)
{
    std::fprintf(stderr, "[p5gui] set_parent api=%s\n", window ? window->api : "(null)");
    auto *c = ctxFrom(p);
    if (!c || !window || std::strcmp(window->api, CLAP_WINDOW_API_COCOA) != 0) return false;
    if (c->view) return true;                        // already attached
    return c->attachTo((NSView *)window->cocoa);
}
static bool gui_set_transient(const clap_plugin_t *, const clap_window_t *) { return false; }
static void gui_suggest_title(const clap_plugin_t *, const char *) {}
static bool gui_show(const clap_plugin_t *p)
{
    std::fprintf(stderr, "[p5gui] show\n");
    auto *c = ctxFrom(p);
    if (!c || !c->view) return false;
    [c->view setHidden:NO];
    c->mapped = true;
    registerTimer(*c);
    [c->view setNeedsDisplay:YES];
    return true;
}
static bool gui_hide(const clap_plugin_t *p)
{
    auto *c = ctxFrom(p);
    if (!c || !c->view) return false;
    unregisterTimer(*c);
    [c->view setHidden:YES];
    c->mapped = false;
    return true;
}

const clap_plugin_gui_t kGui = {
    gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy,
    gui_set_scale, gui_get_size, gui_can_resize, gui_get_resize_hints,
    gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient,
    gui_suggest_title, gui_show, gui_hide,
};

// timer-support: bookkeeping + repaint for hosts that fire it
static void gui_on_timer(const clap_plugin_t *p, clap_id)
{
    if (auto *c = ctxFrom(p))
    {
        c->tick();
        [c->view setNeedsDisplay:YES];
    }
}
const clap_plugin_timer_support_t kTimerSupport = { gui_on_timer };

} // namespace pw
