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
    // voice trim page: [lane][osc][scale,offset]
    bool trimOpen = false;
    // microtuning editor (TUNING): octave window of the dev table + picker + bend ranges
    bool tuneOpen = false, tunePick = false;
    char tuneName[40] = "12-TET";
    short tuneDev[128] = {};
    const float *wheelC = nullptr;         // cumulative cents per tone (Surge-style dial)
    int wheelN = 0, wheelHot = -1;         // tone count incl. octave; hovered/dragged tone (1..N)
    float tuneUp = 2.f, tuneDn = 2.f;           // editor default ranges, semitones
    int tuneRoot = 0;                           // scale root key position (C..B)
    int tuneFocus = 0;                          // which bend-range rail the arrows adjust
    const char *tuneRows[8] = {};               // "standard" + Tunings files
    int tuneRowCount = 0, tunePage = 0, tunePages = 1, tuneSel = -1;
    int tuneTab = 0;                            // picker: 0 stock library, 1 user files
    short trimV[32] = {};
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

// voice V/OCT trim page (EDIT): 8 rows x 4 rails — per OSCILLATOR:
// cols 0,1 = OSC A scale/offset; cols 2,3 = OSC B scale/offset
namespace trim {
constexpr float x = 190, y = 20, w = 1180, h = 408;
constexpr int rows = 8;
inline float rowTop(int i) { return y + 72 + i * 40; }
constexpr int ncols = 4;
constexpr float railX[4] = { 290, 570, 860, 1140 };
constexpr float railW = 200;
bool inside(float lx, float ly);
int hitBar(float lx, float ly, int *row, int *col);   // 1 hit (col may be -1), 0 miss
inline float barX(int col) { return railX[col]; }
inline float barW(int col) { return railW; }
inline float handleX(int col, int v) { return railX[col] + (float)v * (railW * (1.0f / 127.0f)); }
inline int valueAt(int col, float lx)
{
    float t = (lx - railX[col]) / railW;
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    return (int)(t * 127.0f + 0.5f);
}
}

// ---- microtuning editor page geometry (renderer draws, gui_x11 hit-tests) ----
namespace tune {
constexpr float x = 250, y = 20, w = 1060, h = 408;
// bend-range rails across the top, stacked
constexpr float railX = 430, railW = 260, railY = 76, railY2 = 106;
inline float railHandleX(int k, float v) { return railX + (v / 24.f) * railW; }
float railValAt(int k, float lx);               // 0..24 semitones, clamped
bool inside(float lx, float ly);
// root spinner (top right)
constexpr float spinY = 91, spinMinusX = 940, spinPlusX = 1150, spinBoxX = 970, spinBoxW = 150;
// polar dial (bottom left)
constexpr float wheelCX = 490, wheelCY = 276, wheelR = 98;
// chromatic text panel (bottom right): key / Hz / cents rows
constexpr int rows = 12;
constexpr float tblX = 775, tblTop = 158, tblRowH = 19;
// action buttons under the table
constexpr float btnY = 392, btnH = 24;
constexpr float loadX = 775, loadW = 120, saveX = 915, saveW = 120, resetX = 1055, resetW = 120;
inline float railHandleX2(float v) { return railX + (v / 24.f) * railW; }
float etCents(const float *c, int n);           // cents of one equal step
float ringScale(const float *c, int n);         // px per ET unit, auto-ranged like Surge
void nodePos(const float *c, int n, int i, float &x, float &y);   // tone i (1..n)
int nodeAt(const float *c, int n, float lx, float ly);            // nearest node within 10 px, else -1
float clampTone(const float *c, int n, int i, float v);           // strictly monotone neighbours
int hitControl(float lx, float ly);   // 1 LOAD 2 RESET 3/4 rails 5 SAVE 6 root- 7 root+ 0 miss
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
