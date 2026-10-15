#include "p5_gui.hpp"
#include "p5_fontdata.h"

#include <nanovg.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace p5ui {

namespace {

inline NVGcolor col(uint32_t rgb, int a = 255)
{
    return nvgRGBA((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, (uint8_t)a);
}

double trackedWidth(const Gui &g, const char *txt, double track)
{
    double w = 0;
    for (const char *p = txt; *p; p++)
    {
        w += nvgTextBounds(g.vg, 0, 0, p, p + 1, nullptr);
        if (p[1]) w += track;
    }
    return w;
}

// center-aligned text with letter-spacing, like the SVG's text-anchor:middle
void tracked(const Gui &g, float x, float y, const char *txt, float track,
             NVGcolor c)
{
    const double w = trackedWidth(g, txt, track);
    double pen = x - w / 2;
    nvgFillColor(g.vg, c);
    for (const char *p = txt; *p; p++)
    {
        nvgText(g.vg, (float)pen, y, p, p + 1);
        pen += nvgTextBounds(g.vg, 0, 0, p, p + 1, nullptr) + track;
    }
}

void knobScale(const Gui &g, float cx, float cy)
{
    nvgStrokeColor(g.vg, col(kColScale));
    nvgStrokeWidth(g.vg, 0.8f);
    nvgFillColor(g.vg, col(kColScale));
    nvgFontSize(g.vg, kTxtSize[T_SCALE]);
    for (int v = 0; v < 11; v++)
    {
        const double a = (-140.0 + 28.0 * v) * 3.14159265358979 / 180.0;
        const double s = std::sin(a), c = std::cos(a);
        nvgBeginPath(g.vg);
        nvgMoveTo(g.vg, cx + (float)(16.5 * s), cy + (float)(-16.5 * c));
        nvgLineTo(g.vg, cx + (float)(19.5 * s), cy + (float)(-19.5 * c));
        nvgStroke(g.vg);
        char d[2] = {(char)('0' + v), 0};
        tracked(g, cx + (float)(25.2 * s), cy + (float)(-25.2 * c + 1.8), d, 0.0f, col(kColScale));
    }
}

void knobDial(const Gui &g, float cx, float cy, double norm, bool white)
{
    const double deg = kKnobLoDeg + norm * (kKnobHiDeg - kKnobLoDeg);
    const double rad = deg * 3.14159265358979 / 180.0;
    nvgBeginPath(g.vg);
    nvgCircle(g.vg, cx, cy, (float)kKnobR);
    nvgFillColor(g.vg, col(white ? kColDialWhite : kColDial));
    nvgFill(g.vg);
    nvgStrokeColor(g.vg, col(white ? kColDialWhiteRing : kColDialRing));
    nvgStrokeWidth(g.vg, 1.0f);
    nvgStroke(g.vg);
    nvgStrokeColor(g.vg, col(white ? kColPointerDark : kColPointer));
    nvgStrokeWidth(g.vg, 2.0f);
    nvgLineCap(g.vg, NVG_ROUND);
    nvgBeginPath(g.vg);
    // pointer from r=3 to r=14 along the angle (measured from 12 o'clock)
    nvgMoveTo(g.vg, cx + (float)(3.0 * std::sin(rad)), cy - (float)(3.0 * std::cos(rad)));
    nvgLineTo(g.vg, cx + (float)(14.0 * std::sin(rad)), cy - (float)(14.0 * std::cos(rad)));
    nvgStroke(g.vg);
    nvgLineCap(g.vg, NVG_BUTT);
}

void button(const Gui &g, const Btn &b, bool lit)
{
    uint32_t fillc, strokec;
    switch (b.color)
    {
        case BC_GRAY:   fillc = kColBtnGray;   strokec = kColBtnGrayStroke;   break;
        case BC_ORANGE: fillc = kColBtnOrange; strokec = kColBtnOrangeStroke; break;
        default:        fillc = kColBtnDark;   strokec = kColBtnDarkStroke;   break;
    }
    nvgBeginPath(g.vg);
    nvgRoundedRect(g.vg, b.x - kBtnW / 2, b.y - kBtnH / 2, kBtnW, kBtnH, 2.5f);
    nvgFillColor(g.vg, col(fillc));
    nvgFill(g.vg);
    nvgStrokeColor(g.vg, col(strokec));
    nvgStrokeWidth(g.vg, 1.0f);
    nvgStroke(g.vg);

    // LED at (0,-8) r=2.6 relative to center
    nvgBeginPath(g.vg);
    nvgCircle(g.vg, b.x, b.y - 8.0f, 2.6f);
    nvgFillColor(g.vg, col(lit && b.led ? kColLedOn : kColLedOff));
    nvgFill(g.vg);

    if (b.glyph == G_NONE) return;
    nvgStrokeColor(g.vg, col(kColInk));
    nvgStrokeWidth(g.vg, 1.3f);
    nvgBeginPath(g.vg);
    const float gx = b.x, gy = b.y - 24.0f;
    switch (b.glyph)
    {
        case G_SAW:
            nvgMoveTo(g.vg, gx - 6, gy + 3); nvgLineTo(g.vg, gx + 6, gy - 3);
            nvgLineTo(g.vg, gx + 6, gy + 3); break;
        case G_TRI:
            nvgMoveTo(g.vg, gx - 6, gy + 3); nvgLineTo(g.vg, gx, gy - 3);
            nvgLineTo(g.vg, gx + 6, gy + 3); break;
        case G_SQ:
            nvgMoveTo(g.vg, gx - 6, gy + 3); nvgLineTo(g.vg, gx - 6, gy - 3);
            nvgLineTo(g.vg, gx + 2, gy - 3); nvgLineTo(g.vg, gx + 2, gy + 3); break;
    }
    nvgStroke(g.vg);
}

} // namespace

Gui create(NVGcontext *vg)
{
    Gui g;
    g.vg = vg;
    g.fontId = nvgCreateFontMem(vg, "panel", (unsigned char *)kFontNarrowBold, (int)kFontNarrowBoldLen, 0);
    g.screenId = nvgCreateFontMem(vg, "screen", (unsigned char *)kFontScreen, (int)kFontScreenLen, 0);
    return g;
}

void draw(const Gui &g, const State &s, float w, float h)
{
    // Fit the layout into the window preserving aspect; any letterbox margins
    // go symmetrically around the panel, never stacked under it.
    float sc, xoff, yoff;
    fitWindow(w, h, sc, xoff, yoff);

    nvgFontFaceId(g.vg, g.fontId);
    nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);  // y values are baselines, as in the SVG

    // The whole window gets the panel colour first: a host that ignores the
    // resize hints and hands us a tall container gets matching surface, not
    // black letterbox bars.
    nvgBeginPath(g.vg);
    nvgRect(g.vg, 0, 0, w, h);
    nvgFillColor(g.vg, col(kColPanel));
    nvgFill(g.vg);

    nvgSave(g.vg);
    nvgTranslate(g.vg, xoff, yoff);
    nvgScale(g.vg, sc, sc);

    nvgBeginPath(g.vg);
    nvgRect(g.vg, 0, 0, kPanelW, kPanelH);
    nvgFillColor(g.vg, col(kColPanel));
    nvgFill(g.vg);

    // section boxes (headers drawn with the silkscreen patch behind)
    nvgStrokeColor(g.vg, col(kColBox));
    for (const auto &b : kBoxes)
    {
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, b.x, b.y, b.w, b.h, 8.0f);
        nvgStrokeWidth(g.vg, 1.2f);
        nvgStroke(g.vg);
        if (!b.header[0]) continue;
        nvgFontSize(g.vg, kTxtSize[T_HDR]);
        const double tw = trackedWidth(g, b.header, kTxtTrack[T_HDR]);
        nvgBeginPath(g.vg);
        nvgRect(g.vg, b.x + b.w / 2 - tw / 2 - 4, b.y - 6, tw + 8, 12);
        nvgFillColor(g.vg, col(kColPanel));
        nvgFill(g.vg);
        nvgFontSize(g.vg, kTxtSize[T_HDR]);
        tracked(g, b.x + b.w / 2, b.y + 3.6f, b.header, kTxtTrack[T_HDR], col(kColInk));
    }

    // rules
    nvgStrokeColor(g.vg, col(kColBox));
    nvgStrokeWidth(g.vg, 1.0f);
    for (const auto &r : kRules)
    {
        nvgBeginPath(g.vg);
        nvgMoveTo(g.vg, r.x1, r.y1);
        nvgLineTo(g.vg, r.x2, r.y2);
        nvgStroke(g.vg);
    }

    // programmer display
    for (const auto &d : kDisplays)
    {
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, d.x, d.y, d.w, d.h, 2.0f);
        nvgFillColor(g.vg, col(kColDispBg));
        nvgFill(g.vg);
        nvgStrokeColor(g.vg, col(kColDispStroke));
        nvgStrokeWidth(g.vg, 1.0f);
        nvgStroke(g.vg);
        // Retro terminal readout (VT323): patch name + bank-program, amber on dark.
        nvgFontFaceId(g.vg, g.screenId);
        char bp[16] = "";
        if (s.bank > 0) std::snprintf(bp, sizeof bp, "%d-%d", s.bank, s.program);
        float fs = d.h * 0.46f;
        nvgFontSize(g.vg, fs);
        while (fs > 6.0f && nvgTextBounds(g.vg, 0, 0, s.name, s.name + std::strlen(s.name), nullptr) > d.w - 8)
        { fs -= 0.5f; nvgFontSize(g.vg, fs); }
        {
            const float nw = nvgTextBounds(g.vg, 0, 0, s.name, s.name + std::strlen(s.name), nullptr);
            nvgFillColor(g.vg, col(kColDispText));
            nvgText(g.vg, d.x + (float)((d.w - nw) / 2), d.y + d.h * (bp[0] ? 0.44f : 0.72f), s.name, nullptr);
        }
        if (bp[0])
        {
            nvgFontSize(g.vg, d.h * 0.42f);
            const float bw = nvgTextBounds(g.vg, 0, 0, bp, bp + std::strlen(bp), nullptr);
            nvgFillColor(g.vg, col(kColDispText));
            nvgText(g.vg, d.x + (float)((d.w - bw) / 2), d.y + d.h * 0.92f, bp, nullptr);
        }
        nvgFontFaceId(g.vg, g.fontId);
    }

    // knobs
    for (int i = 0; i < kNumKnobs; i++)
    {
        const auto &k = kKnobs[i];
        knobScale(g, k.x, k.y);
        knobDial(g, k.x, k.y, s.knob[i], k.white);
    }

    // buttons
    for (int i = 0; i < kNumBtns; i++)
        button(g, kBtns[i], s.led[i]);

    // silkscreen texts (labels, subheads) — headers drawn with the boxes
    for (const auto &t : kTexts)
    {
        if (t.style == T_HDR) continue; // box headers are drawn above
        nvgFontSize(g.vg, kTxtSize[t.style]);
        tracked(g, t.x, t.y, t.txt, kTxtTrack[t.style], col(kColInk));
    }

    nvgRestore(g.vg);
}

namespace {
inline void toLayout(float w, float h, float px, float py, float &lx, float &ly)
{
    float sc, xoff, yoff;
    fitWindow(w, h, sc, xoff, yoff);
    lx = (px - xoff) / sc;
    ly = (py - yoff) / sc;
}
}

int knobAt(const State &, float px, float py, float w, float h)
{
    float lx, ly;
    toLayout(w, h, px, py, lx, ly);
    for (int i = 0; i < kNumKnobs; i++)
    {
        const float dx = lx - kKnobs[i].x, dy = ly - kKnobs[i].y;
        if (dx * dx + dy * dy <= 19.0f * 19.0f) return i;
    }
    return -1;
}

int btnAt(const State &, float px, float py, float w, float h)
{
    float lx, ly;
    toLayout(w, h, px, py, lx, ly);
    for (int i = 0; i < kNumBtns; i++)
    {
        if (std::fabs(lx - kBtns[i].x) <= kBtnW / 2 + 2 &&
            std::fabs(ly - kBtns[i].y) <= kBtnH / 2 + 2)
            return i;
    }
    return -1;
}

} // namespace p5ui
