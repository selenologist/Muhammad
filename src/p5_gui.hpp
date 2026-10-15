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
    bool   led[kNumBtns] = {};
    char   name[21] = "Peace Be Upon Him";
    int bank = 1, program = 1;     // shown on the programmer display
    const char *build = nullptr;   // version+hash, drawn dim in the panel corner
    const char *diag = nullptr;    // live gui diagnostics (host service, negotiation, size)
    // text-entry modal over the programmer (RECORD save / PRESET assign)
    const char *modalTitle = nullptr;
    const char *modalPrompt = nullptr;
    const char *modalText = nullptr;
    int modalLen = 0;
    char bp[24] = "";              // prebuilt bank-program line ("1-2", "1-2 (3)", "(3)")
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
    yoff = (h - sc * kPanelH) / 2;
}

// Draw the panel filling w x h pixels; scales layout (1560x416) uniformly.
void draw(const Gui &g, const State &s, float w, float h);

// Hit-testing in the same pixel space. Returns layout index or -1.
int knobAt(const State &s, float px, float py, float w, float h);
int btnAt(const State &s, float px, float py, float w, float h);

// Knob drag: fractional range per 100 px of vertical movement.
inline constexpr double kDragRangePer100px = 0.35;
// Which param id a control maps to (for gesture dispatch).
inline int knobParam(int idx) { return kKnobs[idx].pid; }
inline int btnParam(int idx) { return kBtns[idx].pid; }

} // namespace p5ui
