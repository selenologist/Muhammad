// SPDX-License-Identifier: GPL-3.0-only
// clap-gui backend (cocoa): an NSView embedded in the host's view, NSOpenGL
// core-profile context + NanoVG GL3. Same neutral core (gui_core.*) as the
// X11 and Win32 shims: this file owns the view, the GL context and the
// NSEvent -> on* translation. The CLAP cocoa gui API uses LOGICAL points for
// create/get/set size; our render space is physical px (logical * backing
// scale), matching the X11 convention the core expects for drag sensitivity.
//
// MRC (no ARC): we alloc/init the view and release it in teardown. The view
// CLASS itself is allocated at runtime with a per-build unique name (see the
// S7 block below) - there is deliberately no fixed-name @interface/@implementation.
#include <cstdio>
#include <cstring>
#include <cstdlib>          // arc4random (per-build class-name suffix, S7)
#include <string>

#include <objc/runtime.h>   // objc_allocateClassPair, class_addIvar, class_addMethod

#include <Cocoa/Cocoa.h>
#include <OpenGL/gl.h>

#include <nanovg.h>
#define NANOVG_GL3 1        // nanovg_gl.h's GL3 API is behind this guard (same as gui_x11)
#include <nanovg_gl.h>

#include "gui_core.hpp"
#include "muhammad_plugin.hpp"
#include "p5_gui.hpp"

#ifndef P5_VERSION
#define P5_VERSION "dev"    // CMake stamps it per target; keeps the file bare-buildable
#endif

namespace pw { struct Ctx; }            // fwd (referenced by the ivar accessors below)

// ---------------------------------------------------------------------------
// S7: the view class has NO compile-time ObjC name any more. The panel is
// statically embedded in every format (CLAP + VST3 + AU + standalone): with a
// fixed @interface name (the old P5PanelView) a host that loads two of these
// binaries gets "Class P5PanelView is implemented in both...", and one
// module's views can run the OTHER module's methods against a different Ctx
// layout - memory corruption or a jump into unmapped code after an unload.
// Compile-time token pasting (##P5_VERSION) is impossible in this file:
// CMake defines P5_VERSION as a *string literal* ("0.2.3" - quotes and dots,
// not pasteable into an identifier), and this shim must not require a CMake
// change. So the class is allocated at runtime with objc_allocateClassPair
// under a name that embeds the version plus an arc4random suffix: every load
// of every build gets its own unique runtime name, and the two formats can
// never collide. The ivars are added with class_addIvar BEFORE registration
// (the supported ivar path for runtime classes; no KVC/objc_property is
// needed because nothing uses property syntax) and then read/written through
// cached byte offsets.
// ---------------------------------------------------------------------------
static Class   g_panelCls = Nil;        // built once per image per process
static size_t  g_offCore = 0;           // ivar byte offsets (set at class build)
static size_t  g_offTrack = 0;

static inline pw::Ctx *p5Core(id v)
{   return *reinterpret_cast<pw::Ctx **>((unsigned char *)(void *)v + g_offCore); }
static inline void p5SetCore(id v, pw::Ctx *c)
{   *reinterpret_cast<pw::Ctx **>((unsigned char *)(void *)v + g_offCore) = c; }
static inline NSTrackingArea *p5Tracking(id v)
{   return *reinterpret_cast<NSTrackingArea **>((unsigned char *)(void *)v + g_offTrack); }
static inline void p5SetTracking(id v, NSTrackingArea *t)
{   *reinterpret_cast<NSTrackingArea **>((unsigned char *)(void *)v + g_offTrack) = t; }

// A runtime-allocated class has no lexical superclass, so the old [super ...]
// calls go through the inherited IMPs captured in p5_buildPanelClass().
typedef void (*P5VoidIMP)(id, SEL);
typedef void (*P5EventIMP)(id, SEL, NSEvent *);
typedef void (*P5SizeIMP)(id, SEL, NSSize);
static P5VoidIMP  g_superUpdateTracking = nullptr;
static P5VoidIMP  g_superDealloc = nullptr;
static P5EventIMP g_superKeyDown = nullptr;
static P5SizeIMP  g_superSetFrameSize = nullptr;

static Class p5_buildPanelClass(void);  // defined after the event IMPs below

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

// ---- NSPasteboard (debug COPY) ----------------------------------------------
static bool macCopyText(const char *t)
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    NSString *str = [NSString stringWithUTF8String:t];
    return str && [pb setString:str forType:NSPasteboardTypeString];
}

struct Ctx : GuiCore
{
    explicit Ctx(Plugin *p) : GuiCore(p) {}

    NSView *parent = nil;               // borrowed (host-owned)
    NSView *view = nil;                 // runtime panel class (S7); we retain (alloc), release in teardown
    NSOpenGLContext *glctx = nil;       // we retain
    NVGcontext *vg = nullptr;
    p5ui::Gui gui{};
    bool hasGui = false;
    unsigned lastAskMs = 0;
    double scrollAccum = 0.0;           // trackpad notch accumulator (5.12)
    uint32_t lastChangeTick = 0;        // cached Plugin::changeTick_ (4.5-mac)
    long lastRepaintMs = 0;             // forced-repaint stamp (4.5-mac)

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
        if (view)  { p5SetCore(view, nullptr);    // S8: the view may outlive this Ctx;
                                                   // a nil core_ makes every late
                                                   // event a no-op instead of a UAF
                   [view removeFromSuperview]; [view release]; view = nil; }
        parent = nil;
        mapped = false;
    }

    void requestRedraw() { if (view) [view setNeedsDisplay:YES]; }

    // 4.5-mac overdraw: the old timer marked the view dirty every tick
    // (~16 ms), so flushBuffer (swap interval 1) blocked the host's main
    // thread for up to a full frame per open instance even with nothing
    // moving. Repaint only when the plugin state actually moved (changeTick_
    // is bumped by every main-side mutation and merged publish - VU/voice
    // displays ride it), with a ~500 ms forced frame as a safety net.
    void repaintIfStale()
    {
        if (!view) return;
        const uint32_t t = plugin ? plugin->changeTick_.load(std::memory_order_relaxed) : 0u;
        const long now = (long)([NSProcessInfo processInfo].systemUptime * 1000.0);
        if (t != lastChangeTick)
        {   lastChangeTick = t; lastRepaintMs = now; [view setNeedsDisplay:YES]; }
        else if ((unsigned long)(now - lastRepaintMs) >= 500u)
        {   lastRepaintMs = now; [view setNeedsDisplay:YES]; }
    }

    // cocoa has no WM: trim OUR NSWindow the same way the X11 shim trims the
    // host top-level, keeping chrome height.
    void trimWindowIfTall()
    {
        NSWindow *win = view.window;
        if (!win || topTries >= 10 || !coreShouldTrimNow(*this, (long)([NSProcessInfo processInfo].systemUptime * 1000.0)))
            return;
        const double sc = s();
        const long wantH = (long)(view.bounds.size.width * sc * p5ui::kPanelH / p5ui::kPanelW);
        const long curH  = (long)(view.bounds.size.height * sc + 0.5);
        if (curH <= wantH + 40) return;
        const long t = (long)([NSProcessInfo processInfo].systemUptime * 1000.0);
        if ((unsigned long)(t - (long)topNextMs) < 1500u) return;
        topNextMs = (long)t;
        topTries++;
        const NSSize fs = win.frame.size;
        const NSSize cs = win.contentView ? win.contentView.bounds.size : fs;
        const double chrome = fs.height - cs.height;
        const double newH = (double)wantH / sc + chrome;
        std::fprintf(stderr, "[p5gui] host kept view %lx%lx; trimming window %.0fx%.0f -> %.0fx%.0f (try %d)\n",
                     (unsigned long)view.bounds.size.width, (unsigned long)view.bounds.size.height,
                     fs.width, fs.height, fs.width, newH, topTries);
        [win setContentSize:NSMakeSize(fs.width, newH)];
    }

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
        const Class cls = p5_buildPanelClass();      // unique per-build runtime name (S7)
        if (!cls) { std::fprintf(stderr, "[p5gui] cannot register panel class\n"); return false; }
        view = [[cls alloc] initWithFrame:host.bounds];
        p5SetCore(view, this);                       // set on attach; nil-safe everywhere
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        view.wantsLayer = YES;
        view.wantsBestResolutionOpenGLSurface = YES; // 5.12: full-res GL surface on Retina
        [host addSubview:view];
        if (NSRunningApplication *me = NSWorkspace.sharedWorkspace.frontmostApplication)
            std::snprintf(hostName, sizeof hostName, "%.64s",
                          me.localizedName.UTF8String ?: "mac host");
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
        trimWindowIfTall();
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

} // namespace pw

// ---------------------------------------------------------------------------
// the view: translate Cocoa events into the neutral core API. The class is
// runtime-allocated (S7, see the block at the top of this file), so instead
// of an @implementation its methods are plain C functions with the
// (id self, SEL _cmd, ...) ABI, installed via class_addMethod in
// p5_buildPanelClass(); the former [super ...] calls go through the captured
// inherited IMPs. Objective-C function bodies may only reference pw symbols
// through the namespace, so a file-scope using-directive keeps every method
// body byte-identical to the previous @implementation (eventPx,
// onPointerPress, renderFrame, GuiKey... all live in pw).
// ---------------------------------------------------------------------------
using namespace pw;

// mirrors gui_x11 Button3 / gui_win32 WM_RBUTTONDOWN: onRightPress owns the
// press when the hit target supports the polarity flip (the README headline
// feature - it never ran on mac until now); otherwise it degrades to a plain
// right-modified press, exactly like the other two shims.
static bool p5_rightPress(Ctx &c, NSEvent *e, float px, float py)
{
    if (onRightPress(c, px, py)) return true;        // the flip itself repaints the dial
    return onPointerPress(c, px, py, P5_MOD_RIGHT, (unsigned)(e.timestamp * 1000.0));
}

static BOOL p5_isFlipped(id self, SEL _cmd) { (void)self; (void)_cmd; return YES; }  // top-left, like X11
static BOOL p5_acceptsFirstResponder(id self, SEL _cmd) { (void)self; (void)_cmd; return YES; }
static BOOL p5_acceptsFirstMouse(id self, SEL _cmd, NSEvent *e) { (void)self; (void)_cmd; (void)e; return YES; }

static void p5_updateTrackingAreas(id self, SEL _cmd)
{
    if (g_superUpdateTracking) g_superUpdateTracking(self, _cmd);   // was [super updateTrackingAreas]
    NSView *v = (NSView *)self;
    NSTrackingArea *t = p5Tracking(self);
    if (t) { [v removeTrackingArea:t]; [t release]; p5SetTracking(self, nil); }
    t = [[NSTrackingArea alloc]
         initWithRect:v.bounds
              options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect
                owner:v userInfo:nil];
    p5SetTracking(self, t);
    [v addTrackingArea:t];
}

static void p5_viewDidMoveToWindow(id self, SEL _cmd)
{
    Ctx *core_ = p5Core(self);
    NSView *v = (NSView *)self;
    if (core_ && v.window) [v.window makeFirstResponder:v];
}

static void p5_drawRect(id self, SEL _cmd, NSRect rect)
{
    (void)rect;
    NSView *v = (NSView *)self;
    Ctx *core_ = p5Core(self);
    if (!core_ || !core_->vg) return;
    core_->syncPixels();
    [core_->glctx setView:v];
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

static void p5_setFrameSize(id self, SEL _cmd, NSSize newSize)
{
    if (g_superSetFrameSize) g_superSetFrameSize(self, _cmd, newSize);   // was [super setFrameSize:]
    Ctx *core_ = p5Core(self);      // NSView never calls viewDidLayout -
    if (core_) core_->syncPixels(); // setFrameSize is the geometry hook
}

static void p5_mouseDown(id self, SEL _cmd, NSEvent *e)
{
    Ctx *core_ = p5Core(self);
    if (!core_) return;
    float px, py; eventPx((NSView *)self, e, px, py);
    // double-click: NOT a native WM_LBUTTONDBLCLK equivalent here - the core's
    // 450 ms press-to-press detector sees the second press, same as X11.
    // 5.12: Ctrl-click is macOS's portable right button -> same polarity-flip
    // route as rightMouseDown: (the old code ignored controlKeyMask).
    const bool rightish = (e.modifierFlags & NSEventModifierFlagControl) != 0;
    const bool dirty = rightish
        ? p5_rightPress(*core_, e, px, py)
        : onPointerPress(*core_, px, py, eventMods(e), (unsigned)(e.timestamp * 1000.0));
    if (dirty) [self setNeedsDisplay:YES];
    core_ = p5Core(self);                             // S8: press may re-enter the host
    if (core_) core_->consumeFocus();
}

static void p5_mouseUp(id self, SEL _cmd, NSEvent *e)
{
    Ctx *core_ = p5Core(self);
    if (!core_) return;
    float px, py; eventPx((NSView *)self, e, px, py);
    if (onPointerRelease(*core_, px, py, eventMods(e))) [self setNeedsDisplay:YES];
    core_ = p5Core(self);                             // S8: release may re-enter the host
    if (core_) core_->consumeFocus();
}

static void p5_mouseMovedLike(id self, NSEvent *e)
{
    Ctx *core_ = p5Core(self);
    if (!core_) return;
    float px, py; eventPx((NSView *)self, e, px, py);
    if (onPointerMotion(*core_, px, py, eventMods(e))) [self setNeedsDisplay:YES];
}

static void p5_mouseDragged(id self, SEL _cmd, NSEvent *e) { p5_mouseMovedLike(self, e); }
static void p5_mouseMoved(id self, SEL _cmd, NSEvent *e)   { p5_mouseMovedLike(self, e); }

static void p5_rightMouseDown(id self, SEL _cmd, NSEvent *e)
{
    Ctx *core_ = p5Core(self);
    if (!core_) return;
    float px, py; eventPx((NSView *)self, e, px, py);
    if (p5_rightPress(*core_, e, px, py)) [self setNeedsDisplay:YES];
}

static void p5_scrollWheel(id self, SEL _cmd, NSEvent *e)
{
    Ctx *core_ = p5Core(self);
    if (!core_) return;
    // 5.12-mac trackpad fix: the old code sent EVERY deltaY (including 0 -
    // horizontal-only swipes and momentum tails) as an "up" step, pinning
    // knobs at max during a flick. Now a rounded delta of 0 is ignored,
    // momentum inertia is dropped outright, and precise (pixel) deltas
    // consume whole notches only, with the accumulator clamped so a burst
    // can never bank steps between events.
    const double dy = e.deltaY;
    const double ady = dy < 0.0 ? -dy : dy;
    if (ady < 0.5) return;                            // rounds to zero: ignore
    // NSEventMomentumPhaseStationary is the only momentum value a DIRECT
    // wheel/trackpad notch carries; anything else (Began/Changed/Ended/
    // Cancelled) is the inertia stream - drop it outright. (An earlier
    // revision tested a non-existent "Inactive" constant and the mac CI leg
    // caught it: 2026-10-21.)
    if (e.hasPreciseScrollingDeltas &&
        e.momentumPhase != NSEventMomentumPhaseStationary)
        return;                                       // flick tail
    float px, py; eventPx((NSView *)self, e, px, py);
    bool dirty = false;
    if (!e.hasPreciseScrollingDeltas)
    {   // discrete wheel: one notch per event, down = next page = +1, like Button5
        if (onWheel(*core_, px, py, dy < 0.0 ? +1 : -1)) dirty = true;
    }
    else
    {   const double kNotch = 8.0, kCap = 3.0 * kNotch;
        core_->scrollAccum += dy;
        if (core_->scrollAccum > kCap) core_->scrollAccum = kCap;
        else if (core_->scrollAccum < -kCap) core_->scrollAccum = -kCap;
        while (core_->scrollAccum >= kNotch || core_->scrollAccum <= -kNotch)
        {   const int dir = core_->scrollAccum < 0.0 ? +1 : -1;
            if (onWheel(*core_, px, py, dir)) dirty = true;
            core_->scrollAccum -= (dir > 0 ? -kNotch : kNotch);
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

static void p5_keyDown(id self, SEL _cmd, NSEvent *e)
{
    Ctx *core_ = p5Core(self);
    if (!core_) { if (g_superKeyDown) g_superKeyDown(self, _cmd, e); return; }
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
    else if (g_superKeyDown)
    {
        g_superKeyDown(self, _cmd, e);               // was [super keyDown:]: cmd-combos, beeps pass through
        // S8: the host may close the editor synchronously inside the super
        // call - teardown now nils the ivar, so re-read before using it.
        core_ = p5Core(self);
    }
    if (core_) core_->consumeFocus();
}

static void p5_dealloc(id self, SEL _cmd)
{
    NSTrackingArea *t = p5Tracking(self);
    if (t) { [t release]; p5SetTracking(self, nil); }
    // Never touch core_ here: the Ctx may already be destroyed (teardown
    // released our last retain); everything needed is ivar-local.
    if (g_superDealloc) g_superDealloc(self, _cmd);  // was [super dealloc]
}

// Build (once per image) the uniquely-named view class; see S7 up top for
// why the name must never be a fixed compile-time symbol.
static Class p5_buildPanelClass(void)
{
    if (g_panelCls) return g_panelCls;
    char nm[96];
    std::snprintf(nm, sizeof nm, "MuhammadPanelView_%s_%u",
                  P5_VERSION, (unsigned)arc4random());
    Class cls = objc_allocateClassPair((Class)[NSView class], nm, 0);
    if (!cls) return Nil;
    const uint8_t ptrAlign = (uint8_t)(sizeof(void *) == 8 ? 3 : 2);   // log2(alignment)
    if (!class_addIvar(cls, "core_", sizeof(void *), ptrAlign, "^v") ||
        !class_addIvar(cls, "tracking_", sizeof(void *), ptrAlign, "@"))
    {   objc_disposeClassPair(cls); return Nil; }

    // Capture the inherited NSView IMPs BEFORE we shadow them: these are the
    // implementations the old @implementation reached with [super ...].
    g_superUpdateTracking = (P5VoidIMP)class_getMethodImplementation(cls, @selector(updateTrackingAreas));
    g_superSetFrameSize   = (P5SizeIMP)class_getMethodImplementation(cls, @selector(setFrameSize:));
    g_superKeyDown        = (P5EventIMP)class_getMethodImplementation(cls, @selector(keyDown:));
    g_superDealloc        = (P5VoidIMP)class_getMethodImplementation(cls, @selector(dealloc));

    // @encode over the real function signatures gives the runtime a correct
    // method encoding without hand-writing NSRect/NSSize struct encodings.
    class_addMethod(cls, @selector(isFlipped),             (IMP)p5_isFlipped,            @encode(BOOL(id, SEL)));
    class_addMethod(cls, @selector(acceptsFirstResponder), (IMP)p5_acceptsFirstResponder, @encode(BOOL(id, SEL)));
    class_addMethod(cls, @selector(acceptsFirstMouse:),    (IMP)p5_acceptsFirstMouse,    @encode(BOOL(id, SEL, NSEvent *)));
    class_addMethod(cls, @selector(updateTrackingAreas),   (IMP)p5_updateTrackingAreas,  @encode(void(id, SEL)));
    class_addMethod(cls, @selector(viewDidMoveToWindow),   (IMP)p5_viewDidMoveToWindow,  @encode(void(id, SEL)));
    class_addMethod(cls, @selector(drawRect:),             (IMP)p5_drawRect,             @encode(void(id, SEL, NSRect)));
    class_addMethod(cls, @selector(setFrameSize:),         (IMP)p5_setFrameSize,         @encode(void(id, SEL, NSSize)));
    class_addMethod(cls, @selector(mouseDown:),            (IMP)p5_mouseDown,            @encode(void(id, SEL, NSEvent *)));
    class_addMethod(cls, @selector(mouseUp:),              (IMP)p5_mouseUp,              @encode(void(id, SEL, NSEvent *)));
    class_addMethod(cls, @selector(mouseDragged:),         (IMP)p5_mouseDragged,         @encode(void(id, SEL, NSEvent *)));
    class_addMethod(cls, @selector(mouseMoved:),           (IMP)p5_mouseMoved,           @encode(void(id, SEL, NSEvent *)));
    class_addMethod(cls, @selector(rightMouseDown:),       (IMP)p5_rightMouseDown,       @encode(void(id, SEL, NSEvent *)));
    class_addMethod(cls, @selector(scrollWheel:),          (IMP)p5_scrollWheel,          @encode(void(id, SEL, NSEvent *)));
    class_addMethod(cls, @selector(keyDown:),              (IMP)p5_keyDown,              @encode(void(id, SEL, NSEvent *)));
    class_addMethod(cls, @selector(dealloc),               (IMP)p5_dealloc,              @encode(void(id, SEL)));

    objc_registerClassPair(cls);
    Ivar ic = class_getInstanceVariable(cls, "core_");
    Ivar it = class_getInstanceVariable(cls, "tracking_");
    if (!ic || !it) { std::fprintf(stderr, "[p5gui] panel ivars missing\n"); return Nil; }
    g_offCore  = (size_t)ivar_getOffset(ic);
    g_offTrack = (size_t)ivar_getOffset(it);
    g_panelCls = cls;
    std::fprintf(stderr, "[p5gui] registered runtime panel class %s\n", nm);
    return cls;
}

namespace pw {   // reopen: Ctx methods and the clap_plugin_gui surface below
                 // (kGui stays inside pw - muhammad_plugin.hpp declares it so)

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
        c.copyText = macCopyText;
        if (s->host && s->host->get_extension)
            c.hostGui = (const clap_host_gui_t *)s->host->get_extension(s->host, CLAP_EXT_GUI);
        std::fprintf(stderr, "[p5gui] host clap.gui service %s\n", c.hostGui ? "present" : "absent");
    }
    if (auto *cc = ctxFrom(p)) if (api) std::strncpy(cc->guiApi, api, sizeof cc->guiApi - 1);
    return true;
}
static void gui_destroy(const clap_plugin_t *p)
{
    auto *s = static_cast<Plugin *>(p->plugin_data);
    if (!s->gui) return;
    Ctx &c = *static_cast<Ctx *>(s->gui);
    unregisterTimer(c);
    c.teardown();                                    // nils view->core_, releases view + glctx
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
    if (auto *cc = ctxFrom(p)) cc->getSizeCalls++;
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

// timer-support: bookkeeping + gated repaint for hosts that fire it (4.5-mac:
// an unconditional setNeedsDisplay every tick made flushBuffer stall the
// host main thread for frames with nothing to show)
static void gui_on_timer(const clap_plugin_t *p, clap_id)
{
    if (auto *c = ctxFrom(p))
    {
        c->tick();
        c->repaintIfStale();
    }
}
const clap_plugin_timer_support_t kTimerSupport = { gui_on_timer };

} // namespace pw
