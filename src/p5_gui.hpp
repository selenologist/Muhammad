// Prophet-5 panel renderer: pure function of (layout, state). Used identically
// by the embedded plugin GUI and the headless golden-image test, so CI proves
// the shipped drawing code, not a reimplementation.
#pragma once

#include "panel_layout.h"

struct NVGcontext;

namespace p5ui {

// Fully resolved view state. The host layer (plugin or test) fills this from
// CLAP params + engine; the renderer stays dumb, so the same struct drives
// Bitwig and the golden PNG.
struct State
{
    double knob[kNumKnobs] = {};   // 0..1 normalized position
    uint8_t led[kNumBtns] = {};   // 0 off, 1 amber, 2 red
    char   name[21] = "Peace Be Upon Him";
    int bank = 1, program = 1;     // shown on the programmer display
    const char *build = nullptr;   // version+hash, drawn dim in the panel corner
    const char *diag = nullptr;    // live gui diagnostics (host service, negotiation, size)
    // text-entry modal over the programmer (RECORD save / PRESET assign)
    // preset picker (programmer PRESET): tabs Factory|User, 10 rows, paged
    const char *pickerTitle = nullptr;   // non-null ONLY while picker is open
    int  pickerTab = 0, pickerPage = 0, pickerPages = 1, pickerCount = 0;
    int  pickerSel = -1;             // row currently matching the loaded patch
    int  pickerBind = -1;            // row whose ASSIGN is armed
    char pickerRows[7][44] = {};
    // voice trim page
    bool trimOpen = false;
    short trimS[8] = {}, trimO[8] = {};
    const char *modalTitle = nullptr;
    const char *modalPrompt = nullptr;
    const char *modalText = nullptr;
    int modalLen = 0;
    char bpSlot[10] = "";          // "b-p" digits (reserved fixed column, centered)
    char bpPend[8] = "";           // pending-bank "(n)" — left of slot, fixed column
    const char *bindHint = nullptr;// "ASSIGN -> PRESS 1..8" while a bind is armed
};

struct Gui
{
    NVGcontext *vg = nullptr;
    int fontId = 0;    // silkscreen (Liberation Sans Narrow Bold)
    int screenId = 0;  // programmer display (VT323 retro terminal font)
};

// Load the embedded font into the context (call once per NVGcontext).
Gui create(NVGcontext *vg);

// Window fit: uniform scale that keeps the 1560x416 layout fully visible,
// centered symmetrically. Shared by draw(), hit tests and the embedders.
inline void fitWindow(float w, float h, float &sc, float &xoff, float &yoff)
{
    const float sx = w / kPanelW, sy = h / kPanelH;
    sc = sx < sy ? sx : sy;
    xoff = (w - sc * kPanelW) / 2;
    // A window taller than the panel is OURS: the panel pins to the top and
    // the remainder is drawn chassis, not a letterbox. Centre only when the
    // height is the binding constraint (short-and-wide containers).
    yoff = (sy >= sx) ? 0.f : (h - sc * kPanelH) / 2;
}

// Draw the panel filling w x h pixels; scales layout (1560x416) uniformly.
void draw(const Gui &g, const State &s, float w, float h);

// Hit-testing in the same pixel space. Returns layout index or -1.
// picker geometry in LAYOUT coords (renderer draws, gui_x11 hit-tests):
namespace pick {
constexpr float x = 430, y = 8, w = 700, h = 400, rowH = 44, rowStep = 49;
constexpr int rows = 7;
inline float rowTop(int i) { return y + 46 + i * rowStep; }
int hitTab(float lx, float ly);      // 0 factory 1 user -1
int hitRow(float lx, float ly);      // 0..9 or -1 (row body, left of the ASSIGN chip)
int hitAssign(float lx, float ly);   // 0..9 ASSIGN chip or -1
inline float assignW() { return 86.f; }
int hitPage(float lx, float ly);     // -1 prev, +1 next, 0 none
bool hitClose(float lx, float ly);
bool inside(float lx, float ly);
}

// voice V/OCT trim page (EDIT): 8 rows x scale/offset bar-sliders
namespace trim {
constexpr float x = 300, y = 20, w = 960, h = 408;
constexpr int rows = 8;
inline float rowTop(int i) { return y + 56 + i * 42; }
constexpr float scX = 410, scW = 350, ofX = 880, ofW = 330;
bool inside(float lx, float ly);
int hitBar(float lx, float ly, int *row, int *col);   // 1 hit, 0 miss (still inside)
inline float barX(int col) { return col ? ofX : scX; }
inline float barW(int col) { return col ? ofW : scW; }
inline float handleX(int col, int v) { return barX(col) + (float)v * (barW(col) * (1.0f / 127.0f)); }
inline int valueAt(int col, float lx)
{
    float t = (lx - barX(col)) / barW(col);
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    return (int)(t * 127.0f + 0.5f);
}
}
void pointToLayout(float px, float py, float w, float h, float &lx, float &ly);
int knobAt(const State &s, float px, float py, float w, float h);
int btnAt(const State &s, float px, float py, float w, float h);

// Knob drag: fractional range per 100 px of vertical movement.
inline constexpr double kDragRangePer100px = 0.35;
// Which param id a control maps to (for gesture dispatch).
inline int knobParam(int idx) { return kKnobs[idx].pid; }
inline int btnParam(int idx) { return kBtns[idx].pid; }

} // namespace p5ui
