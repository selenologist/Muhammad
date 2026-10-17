// SPDX-License-Identifier: GPL-3.0-only
// Platform-neutral GUI core for the muhammad panel: all interaction state,
// the CLAP-param plumbing, the modal/wheel/trim/tune state machines and the
// resize negotiation. The platform shims (gui_x11.cpp, gui_win32.cpp) own the
// window + GL context and translate native events into the on* API below.
// Behavior here must stay EXACTLY as it was inside gui_x11.cpp: the X11 shim
// feeds it raw window-pixel coordinates so knob-drag sensitivity
// (kDragRangePer100px over physical px) and double-click timing are
// byte-identical post-extraction.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "p5_gui.hpp"      // p5ui::State, p5ui::Gui
#include "p5_store.hpp"    // p5store::Source

struct NVGcontext;

namespace pw {

class Plugin;            // muhammad_plugin.hpp (full type needed in gui_core.cpp)

// Portable modifier bits. P5_MOD_SHIFT equals X11's ShiftMask (1u<<0) so the
// X11 shim can pass ev.state straight through; P5_MOD_RIGHT is the sentinel
// the original gui_x11.cpp already used inside actuate() for right-click
// steps. (Named P5_*: MOD_SHIFT is a RegisterHotKey macro in winuser.h.)
constexpr unsigned P5_MOD_SHIFT = 1u;
constexpr unsigned P5_MOD_RIGHT = 1u << 30;

// Portable key codes for the arrow/return/escape/backspace special keys
// (printable characters go through onChar instead).
enum GuiKey : int
{
    KEY_NONE = 0,
    K_UP, K_DOWN, K_LEFT, K_RIGHT,    // (K_* not KEY_*: KEY_UP etc are winuser.h macros)
    K_RETURN, K_ESCAPE, K_BACKSPACE,
};

// ---- shared GUI state (was the neutral half of gui_x11.cpp's struct Ctx) ----

struct GuiCore
{
    explicit GuiCore(Plugin *p) : plugin(p) {}

    Plugin *plugin;

    double scale = 1.0;                 // host set_scale (physical px per layout unit)
    uint32_t w = 0, h = 0;              // current pixel size
    bool mapped = false;

    // drag state
    int dragKnob = -1, pressBtn = -1;

    // text-entry modal: 0 none, 1 save patch (RECORD), 2 assign slot (PRESET)
    int modal = 0;                    // 1 save, 2 assign, 3 overwrite-confirm, 4 picker, 5 trim
    int trimDrag = 0, trimRow = 0, trimCol = 0;
    int pickerTab = 0, pickerPage = 0;
    std::vector<std::string> userRows;   // cached dir listing for the picker
    std::vector<std::string> tuneFiles;  // Tunings dir listing (TUNING page)
    int tuneDrag = 0, tunePage = 0;      // 3/4 = which bend-range rail
    int tuneFocus = 0;                   // arrow-adjusted rail (0 up, 1 down)
    int tuneTab = 0;                     // picker: 0 stock library, 1 user files
    uint16_t wheelMask = 0; int wheelWalk = 0, wheelRoot = 0;
    float wheelC[64] = {};               // live editable scale (cumulative cents)
    int wheelN = 0, wheelHot = -1, wheelDragIdx = 0;
    float wheelDragC0 = 0.f, wheelDragDist0 = 0.f;
    unsigned wheelLastMs = 0; int wheelLastIdx = -1;   // manual double-click detection
    char tuneRowBuf[8][64] = {};
    bool bindPending = false;            // ASSIGN armed: next program 1-8 binds
    p5store::Source bindSrc;
    char modalBuf[32] = {};
    char modalHit[36] = {};             // existing file that the typed name folds to
    int modalLen = 0;
    int modalSlot = 0;
    int dragStartY = 0;                // window-pixel Y of a knob-drag grab
    double dragStartNorm = 0.0;

    clap_id timerId = 0;
    bool timerRegistered = false;

    uint32_t reqW = 0, reqH = 0;   // set_size may arrive before set_parent
    uint32_t parW = 0, parH = 0, ancW = 0, ancH = 0;   // wrapper / ancestor sizes (diag)

    // host-side gui service + aspect negotiation bookkeeping
    const clap_host_gui_t *hostGui = nullptr;
    int resizeRequests = 0;        // give up if the host keeps saying no
    long lastAskMs = 0;

    // keyboard focus request: set when a text-entry modal opens; the shim
    // consumes it (XSetInputFocus / SetFocus) in the same frame.
    bool needFocus = false;

    // debug page (modal 10): platform shims install copyText (CLIPBOARD
    // owner on X11, CF_UNICODETEXT on win32, NSPasteboard on cocoa) and
    // record the api the host asked for.
    bool (*copyText)(const char *text) = nullptr;
    char guiApi[8] = "none";
    const char *modeSource = "embedded";      // who chose embed/float
    char hostName[72] = "unknown host";       // WM_CLASS / window title / frontmost app
    int  askLastRet = -1;
    long askNextMs = 0;                       // ask throttle (1.2 s)
    int  totalAsks = 0;                       // NEVER reset by configure: a host
                                              // that accepts then re-asserts the
                                              // oversized slot must not be asked
                                              // forever (the 1 Hz flicker)
    long acceptedAtMs = 0;                    // stamp of the last ACCEPTED ask
    bool giveUpOnHost = false;                // REFUSED seen; cleared when the
                                              // window finally lands at aspect
    unsigned long trimWid = 0;                // last window we resized
    uint32_t trimAfterW = 0, trimAfterH = 0;  // its geometry AFTER resize (XSync readback)
    int  getSizeCalls = 0;                    // did the host ever ask our size?
    int  trimIneffective = 0;                 // consecutive trims that did not shrink
    int  topTries = 0;                        // top-level (host window) shrink budget
    long topNextMs = 0;
    char dbg[1200] = {};
};

// ---- neutral input API (called by both platform pumps) ----------------------
// px/py are WINDOW-PIXEL coordinates (the same space X11 events carry); layout
// conversion happens inside. Each returns true when the panel must repaint.
// timeMs: monotonic millisecond stamp for the 450 ms manual double-click
// detector (X11: ev.xbutton.time; win32: GetTickCount()).

bool onPointerPress(GuiCore &c, float px, float py, unsigned mods, unsigned timeMs);
bool onPointerMotion(GuiCore &c, float px, float py, unsigned mods);
bool onPointerRelease(GuiCore &c, float px, float py, unsigned mods);
bool onWheel(GuiCore &c, float px, float py, int dir);   // dir: -1 up, +1 down/next page
bool onKey(GuiCore &c, GuiKey key, unsigned mods);       // true = consumed
bool onChar(GuiCore &c, unsigned int ch);                // true = inserted (text modals)

// aspect negotiation. The shim reads the host slot's current client size and
// (for the grow-ask) the ancestor's, then calls these; the core owns the
// "never taller than panel aspect" math, the request_resize budget and the
// 1 s politeness window.
bool coreMirrorSlot(GuiCore &c, int slotW, int slotH, uint32_t &outW, uint32_t &outH,
                    long nowMs);
bool coreShouldTrimNow(GuiCore &c, long nowMs);
void coreAncestorAsk(GuiCore &c, uint32_t parW, uint32_t parH,
                     uint32_t ancW, uint32_t ancH, long nowMs);

// ---- rendering / actions ------------------------------------------------------

void fillState(GuiCore &c, p5ui::State &st);
void renderFrame(GuiCore &c, NVGcontext *vg, p5ui::Gui &gui);   // nvg frame only (no GL calls)
void actuate(GuiCore &c, int btnIdx, unsigned mods = 0);

// clap_host_timer_support registration is pure CLAP -> lives in the core.
void registerTimer(GuiCore &c);
void unregisterTimer(GuiCore &c);

} // namespace pw
