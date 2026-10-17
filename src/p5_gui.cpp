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
    nvgTextLetterSpacing(g.vg, (float)track);
    const float adv = nvgTextBounds(g.vg, 0, 0, txt, nullptr, nullptr);
    nvgTextLetterSpacing(g.vg, 0.f);
    return adv;
}

// center-aligned text: ONE nvgText call so fontstash applies real kern pairs,
// with letter-spacing through the native API (the old per-glyph pen ignored
// kerning entirely and read as bad spacing at most sizes).
void tracked(const Gui &g, float x, float y, const char *txt, float track,
             NVGcolor c)
{
    nvgTextLetterSpacing(g.vg, track);
    nvgFillColor(g.vg, c);
    const float adv = nvgTextBounds(g.vg, 0, 0, txt, nullptr, nullptr);
    nvgText(g.vg, x - adv / 2, y, txt, nullptr);
    nvgTextLetterSpacing(g.vg, 0.f);
}

void knobScale(const Gui &g, float cx, float cy, int tlo, int thi)
{
    const int n = thi - tlo;
    nvgStrokeColor(g.vg, col(kColScale));
    nvgStrokeWidth(g.vg, 0.8f);
    nvgFillColor(g.vg, col(kColScale));
    nvgFontSize(g.vg, kTxtSize[T_SCALE]);
    for (int v = 0; v <= n; v++)
    {
        const double a = (-140.0 + 280.0 * v / n) * 3.14159265358979 / 180.0;
        const double s = std::sin(a), c = std::cos(a);
        nvgBeginPath(g.vg);
        nvgMoveTo(g.vg, cx + (float)(16.5 * s), cy + (float)(-16.5 * c));
        nvgLineTo(g.vg, cx + (float)(19.5 * s), cy + (float)(-19.5 * c));
        nvgStroke(g.vg);
        char d[4];
        std::snprintf(d, sizeof d, "%d", tlo + v);
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

void button(const Gui &g, const Btn &b, int ledv)
{
    uint32_t fillc, strokec;
    switch (b.color)
    {
        case BC_GRAY:   fillc = kColBtnGray;   strokec = kColBtnGrayStroke;   break;
        case BC_ORANGE: fillc = kColBtnOrange; strokec = kColBtnOrangeStroke; break;
        case BC_WHITE:  fillc = kColBtnWhite;  strokec = kColBtnWhiteStroke;   break;
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
    nvgFillColor(g.vg, ledv == 1 ? nvgRGBA(240, 160, 30, 255)              // amber = HALF
                   : (ledv && b.led) ? col(kColLedOn) : col(kColLedOff));  // red = on
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
    g.fontId = nvgCreateFontMem(vg, "panel", (unsigned char *)kFontPanel, (int)kFontPanelLen, 0);
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

    // device-pixel-grid snap for text: a fractional origin makes the bilinear
    // atlas sampling smear an extra half pixel; rounding the DEVICE position
    // first keeps glyph stems on pixel boundaries.
    auto sxp = [&](float v, float dev = 0.f) { return (std::round(xoff + sc * v) + dev - xoff) / sc; };
    auto syp = [&](float v, float dev = 0.f) { return (std::round(yoff + sc * v) + dev - yoff) / sc; };

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
        float fs = d.h * 0.46f;
        nvgFontSize(g.vg, fs);
        while (fs > 6.0f && nvgTextBounds(g.vg, 0, 0, s.name, s.name + std::strlen(s.name), nullptr) > d.w - 8)
        { fs -= 0.5f; nvgFontSize(g.vg, fs); }
        {
            const float nw = nvgTextBounds(g.vg, 0, 0, s.name, s.name + std::strlen(s.name), nullptr);
            nvgFillColor(g.vg, col(kColDispText));
            nvgText(g.vg, sxp(d.x + (float)((d.w - nw) / 2)), syp(d.y + d.h * ((s.bpSlot[0] || s.bpPend[0]) ? 0.44f : 0.72f)), s.name, nullptr);
        }
        if (s.bpSlot[0] || s.bpPend[0])
        {   // two reserved fixed columns: "(n)" then "b-p" — appearance never shifts digits
            nvgFontSize(g.vg, d.h * 0.42f);
            const float pw = nvgTextBounds(g.vg, 0, 0, "(25)", nullptr, nullptr);
            const float sw = nvgTextBounds(g.vg, 0, 0, "25-8", nullptr, nullptr);
            const float gap = d.h * 0.16f;
            const float x0 = d.x + (d.w - (pw + gap + sw)) / 2.f;
            if (s.bpPend[0])
            {
                const float q = nvgTextBounds(g.vg, 0, 0, s.bpPend, s.bpPend + std::strlen(s.bpPend), nullptr);
                nvgFillColor(g.vg, col(kColDispText));
                nvgText(g.vg, sxp(x0 + pw - q), syp(d.y + d.h * 0.92f), s.bpPend, nullptr);   // right-align in column
            }
            if (s.bpSlot[0])
            {
                const float q = nvgTextBounds(g.vg, 0, 0, s.bpSlot, s.bpSlot + std::strlen(s.bpSlot), nullptr);
                nvgFillColor(g.vg, col(kColDispText));
                nvgText(g.vg, sxp(x0 + pw + gap + (sw - q) / 2.f), syp(d.y + d.h * 0.92f), s.bpSlot, nullptr);
            }
        }
        if (s.bindHint)
        {   // armed ASSIGN: flash a line under the readout — press 1..8 to bind
            nvgFontFaceId(g.vg, g.screenId);
            nvgFontSize(g.vg, 13.f);
            nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
            nvgFillColor(g.vg, nvgRGBA(255, 120, 60, 255));
            nvgText(g.vg, sxp(1056.f), syp(372.f), s.bindHint, nullptr);
            nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
            nvgFontFaceId(g.vg, g.fontId);
        }
        nvgFontFaceId(g.vg, g.fontId);
    }

    // knobs
    for (int i = 0; i < kNumKnobs; i++)
    {
        const auto &k = kKnobs[i];
        knobScale(g, k.x, k.y, k.tickLo, k.tickHi);
        knobDial(g, k.x, k.y, s.knob[i], k.white);
    }

    // buttons
    for (int i = 0; i < kNumBtns; i++)
        button(g, kBtns[i], s.led[i]);

    // extra container height below the panel is OURS (the embedder pins the
    // panel to the top and hands us the whole window): chassis + wordmark.
    const float winBot = (h - yoff) / sc, winRight = (w - xoff) / sc;
    const float chassisH = winBot - kPanelH;
    const bool chassis = chassisH > 40.f;
    if (chassis)
    {
        const float wl = -xoff / sc, ww = winRight - wl;      // well with depth, not a hole
        NVGpaint grad = nvgLinearGradient(g.vg, 0, kPanelH, 0, winBot,
                                         nvgRGBA(0, 0, 0, 60), nvgRGBA(0, 0, 0, 22));
        nvgBeginPath(g.vg);
        nvgRect(g.vg, wl, kPanelH, ww, chassisH);
        nvgFillPaint(g.vg, grad);
        nvgFill(g.vg);
        nvgBeginPath(g.vg);                                   // seam under the panel
        nvgMoveTo(g.vg, wl, kPanelH + 0.5f);
        nvgLineTo(g.vg, wl + ww, kPanelH + 0.5f);
        nvgStrokeColor(g.vg, nvgRGBA(0, 0, 0, 140));
        nvgStrokeWidth(g.vg, 1.f);
        nvgStroke(g.vg);
        nvgBeginPath(g.vg);
        nvgMoveTo(g.vg, wl, kPanelH + 1.5f);
        nvgLineTo(g.vg, wl + ww, kPanelH + 1.5f);
        nvgStrokeColor(g.vg, nvgRGBA(255, 255, 255, 12));
        nvgStrokeWidth(g.vg, 1.f);
        nvgStroke(g.vg);
        const float cy = kPanelH + chassisH * 0.42f;
        const float fs = chassisH < 160.f ? 34.f : chassisH < 320.f ? 52.f : 76.f;
        nvgFontFaceId(g.vg, g.fontId);
        nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(g.vg, fs);
        nvgTextLetterSpacing(g.vg, fs * 0.22f);
        const float wx = (winRight - xoff / sc) / 2.f + fs * 0.11f;   // undo trailing-space bias
        nvgFillColor(g.vg, nvgRGBA(0, 0, 0, 150));                     // raised mark: shadow then face
        nvgText(g.vg, wx, cy + fs * 0.05f, "MUHAMMAD", nullptr);
        nvgFillColor(g.vg, nvgRGBA(255, 255, 255, 46));
        nvgText(g.vg, wx, cy, "MUHAMMAD", nullptr);
        nvgTextLetterSpacing(g.vg, 0.f);
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        nvgFontFaceId(g.vg, g.fontId);
    }

    // build stamp (+ diag), bottom-right of the WINDOW — the only way to prove
    // which binary a host actually mapped when its stderr is unreachable
    if (s.build && s.build[0])
    {
        const float right = chassis ? winRight - 10.f : kPanelW - 8.f;
        const float base  = chassis ? winBot - 12.f : kPanelH - 8.f;
        nvgFontSize(g.vg, 11.5f);
        float x = right;
        const float adv = nvgTextBounds(g.vg, 0, 0, s.build, nullptr, nullptr);
        nvgFillColor(g.vg, nvgRGBA(206, 206, 216, 255));
        nvgText(g.vg, x - adv, base, s.build, nullptr);
        x -= adv + 14;
        if (s.diag && s.diag[0])
        {
            const float dadv = nvgTextBounds(g.vg, 0, 0, s.diag, nullptr, nullptr);
            nvgFillColor(g.vg, nvgRGBA(150, 176, 206, 255));   // cool grey-blue, quieter than the stamp
            nvgText(g.vg, x - dadv, base, s.diag, nullptr);
        }
    }

    // silkscreen texts (labels, subheads) — headers drawn with the boxes
    for (const auto &t : kTexts)
    {
        if (t.style == T_HDR) continue; // box headers are drawn above
        nvgFontSize(g.vg, kTxtSize[t.style]);
        tracked(g, t.x, t.y, t.txt, kTxtTrack[t.style], col(kColInk));
    }

    // ---- preset picker (programmer PRESET): Factory|User tabs, paged rows ----
    if (s.pickerTitle && s.pickerTitle[0])
    {
        nvgBeginPath(g.vg);
        nvgRect(g.vg, 0, 0, kPanelW, kPanelH);
        nvgFillColor(g.vg, nvgRGBA(0, 0, 0, 150));
        nvgFill(g.vg);
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, pick::x, pick::y, pick::w, pick::h, 6);
        nvgFillColor(g.vg, col(kColPanel));
        nvgFill(g.vg);
        nvgStrokeColor(g.vg, col(kColBox));
        nvgStrokeWidth(g.vg, 1.5f);
        nvgStroke(g.vg);
        // tabs
        for (int t = 0; t < 2; t++)
        {
            const float tx = pick::x + 12 + t * 150;
            nvgBeginPath(g.vg);
            nvgRoundedRect(g.vg, tx, pick::y + 10, 144, 30, 3);
            nvgFillColor(g.vg, s.pickerTab == t ? col(kColDispBg) : nvgRGBA(52, 52, 58, 255));
            nvgFill(g.vg);
            nvgStrokeColor(g.vg, col(kColDispStroke));
            nvgStrokeWidth(g.vg, 1.f);
            nvgStroke(g.vg);
            nvgFontFaceId(g.vg, g.fontId);
            nvgFontSize(g.vg, 16.f);
            nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(g.vg, s.pickerTab == t ? col(kColDispText) : col(kColInk));
            nvgText(g.vg, sxp(tx + 72), syp(pick::y + 25), t ? "USER" : "FACTORY", nullptr);
        }
        // page controls + close
        const char *pp[2] = { "<", ">" };
        for (int t = 0; t < 2; t++)
        {
            const float px2 = pick::x + pick::w - 172 + t * 50;
            nvgBeginPath(g.vg);
            nvgRoundedRect(g.vg, px2, pick::y + 10, 44, 26, 3);
            nvgFillColor(g.vg, nvgRGBA(52, 52, 58, 255));
            nvgFill(g.vg);
            nvgStrokeColor(g.vg, col(kColDispStroke));
            nvgStroke(g.vg);
            nvgFontSize(g.vg, 11.f);
            nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(g.vg, col(kColInk));
            nvgText(g.vg, px2 + 22, pick::y + 23, pp[t], nullptr);
        }
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, pick::x + pick::w - 44, pick::y + 10, 30, 26, 3);
        nvgFillColor(g.vg, nvgRGBA(90, 40, 36, 255));
        nvgFill(g.vg);
        nvgFontSize(g.vg, 10.f);
        nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(g.vg, col(kColInk));
        nvgText(g.vg, pick::x + pick::w - 29, pick::y + 23, "X", nullptr);
        // target line + rows
        nvgFontSize(g.vg, 14.f);
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        nvgFillColor(g.vg, nvgRGBA(220, 220, 230, 255));
        nvgText(g.vg, sxp(pick::x + 320), syp(pick::y + 28), s.pickerTitle, nullptr);
        char pg[16];
        std::snprintf(pg, sizeof pg, "%d/%d", s.pickerPage + 1, s.pickerPages < 1 ? 1 : s.pickerPages);
        nvgTextAlign(g.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
        nvgFontSize(g.vg, 14.f);
        nvgText(g.vg, sxp(pick::x + pick::w - 182), syp(pick::y + 28), pg, nullptr);
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(g.vg, g.screenId);
        nvgFontSize(g.vg, 30.f);
        for (int i = 0; i < s.pickerCount && i < pick::rows; i++)
        {
            const float ry = pick::rowTop(i);
            nvgBeginPath(g.vg);
            nvgRoundedRect(g.vg, pick::x + 12, ry, pick::w - 24, pick::rowH, 4);
            nvgFillColor(g.vg, i == s.pickerSel ? nvgRGBA(70, 40, 32, 255)
                              : (i & 1) ? nvgRGBA(40, 40, 46, 255) : nvgRGBA(32, 32, 37, 255));
            nvgFill(g.vg);
            if (i == s.pickerSel)
            {
                nvgStrokeColor(g.vg, col(kColDispText));
                nvgStrokeWidth(g.vg, 1.f);
                nvgStroke(g.vg);
            }
            nvgFillColor(g.vg, col(kColDispText));
            nvgFontFaceId(g.vg, g.screenId);
            nvgFontSize(g.vg, 30.f);                      // per row: chip code below changes it
            const char *rt = s.pickerRows[i];
            const float rtx = sxp(pick::x + 26), rty = syp(ry + pick::rowH / 2);
            nvgText(g.vg, rtx, rty, rt, nullptr);
            nvgText(g.vg, sxp(pick::x + 26, 1), rty, rt, nullptr);   // faux-bold: +1 device px
            // ASSIGN chip at the right end of the row
            const float ax = pick::x + pick::w - 12 - pick::assignW();
            nvgBeginPath(g.vg);
            nvgRoundedRect(g.vg, ax, ry + 5, pick::assignW(), pick::rowH - 10, 4);
            nvgFillColor(g.vg, i == s.pickerBind ? nvgRGBA(150, 90, 30, 255) : nvgRGBA(58, 58, 66, 255));
            nvgFill(g.vg);
            nvgStrokeColor(g.vg, i == s.pickerBind ? nvgRGBA(255, 170, 80, 255) : col(kColDispStroke));
            nvgStroke(g.vg);
            nvgFontFaceId(g.vg, g.fontId);
            nvgFontSize(g.vg, 13.f);
            nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(g.vg, col(kColInk));
            nvgText(g.vg, sxp(ax + pick::assignW() / 2), syp(ry + pick::rowH / 2), "ASSIGN", nullptr);
            nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);   // next rows center again
        }
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        nvgFontFaceId(g.vg, g.fontId);
    }

    // ---- voice V/OCT trim page (EDIT) ----
    if (s.trimOpen)
    {
        nvgBeginPath(g.vg);
        nvgRect(g.vg, 0, 0, kPanelW, kPanelH);
        nvgFillColor(g.vg, nvgRGBA(0, 0, 0, 150));
        nvgFill(g.vg);
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, trim::x, trim::y, trim::w, trim::h, 6);
        nvgFillColor(g.vg, col(kColPanel));
        nvgFill(g.vg);
        nvgStrokeColor(g.vg, col(kColBox));
        nvgStrokeWidth(g.vg, 1.5f);
        nvgStroke(g.vg);
        nvgFontFaceId(g.vg, g.fontId);
        nvgFontSize(g.vg, 17.f);
        nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(g.vg, col(kColInk));
        nvgText(g.vg, sxp(trim::x + trim::w / 2), syp(trim::y + 22),
                "VOICE  V/OCT  TRIM   -   PER  OSCILLATOR", nullptr);
        nvgFontSize(g.vg, 12.f);
        static const char *const kCols[4] = { "A SCALE", "A OFFSET", "B SCALE", "B OFFSET" };
        for (int k = 0; k < trim::ncols; k++)
            nvgText(g.vg, sxp(trim::railX[k] + trim::railW / 2), syp(trim::y + 46), kCols[k], nullptr);
        nvgFontSize(g.vg, 11.f);
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(g.vg, sxp(trim::x + 14), syp(trim::y + trim::h - 10),
                "trimmers fixed per oscillator - like under the lid.  right-click centres.", nullptr);
        for (int i = 0; i < trim::rows; i++)
        {
            const float ry = trim::rowTop(i) + 10;
            char lab[8];
            std::snprintf(lab, sizeof lab, "V%d", i + 1);
            nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFillColor(g.vg, col(kColInk));
            nvgText(g.vg, sxp(trim::x + 66), syp(ry), lab, nullptr);
            for (int k = 0; k < trim::ncols; k++)
            {
                const int v = s.trimV[4*i + 2*(k/2) + (k%2)];
                const float bx = trim::railX[k], bw = trim::railW;
                nvgBeginPath(g.vg);
                nvgRect(g.vg, bx, ry - 1.5f, bw, 3);
                nvgFillColor(g.vg, nvgRGBA(70, 70, 78, 255));
                nvgFill(g.vg);
                nvgBeginPath(g.vg);
                nvgRect(g.vg, bx + bw * (64.0f / 127.0f) - 0.75f, ry - 8.f, 1.5f, 16.f);
                nvgFillColor(g.vg, nvgRGBA(120, 120, 130, 255));
                nvgFill(g.vg);
                const float hx = trim::handleX(k, v);
                nvgBeginPath(g.vg);
                nvgCircle(g.vg, hx, ry, 7);
                nvgFillColor(g.vg, v == 64 ? nvgRGBA(150, 150, 158, 255) : nvgRGBA(240, 160, 30, 255));
                nvgFill(g.vg);
                char vt[8];
                std::snprintf(vt, sizeof vt, "%d", v);
                nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(g.vg, col(kColInk));
                nvgText(g.vg, sxp(bx + bw + 7), syp(ry), vt, nullptr);
            }
        }
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    }

    // ---- microtuning editor (TUNING) ----
    if (s.tuneOpen)
    {
        nvgBeginPath(g.vg);
        nvgRect(g.vg, 0, 0, kPanelW, kPanelH);
        nvgFillColor(g.vg, nvgRGBA(0, 0, 0, 150));
        nvgFill(g.vg);
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, tune::x, tune::y, tune::w, tune::h, 6);
        nvgFillColor(g.vg, col(kColPanel));
        nvgFill(g.vg);
        nvgStrokeColor(g.vg, col(kColBox));
        nvgStrokeWidth(g.vg, 1.5f);
        nvgStroke(g.vg);
        nvgFontFaceId(g.vg, g.fontId);
        nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(g.vg, 16.f);
        nvgFillColor(g.vg, col(kColInk));
        nvgText(g.vg, sxp(tune::x + tune::w / 2), syp(tune::y + 20), "MICROTUNING", nullptr);
        nvgFontSize(g.vg, 12.f);
        nvgText(g.vg, sxp(tune::x + tune::w / 2), syp(tune::y + 38), s.tuneName, nullptr);

        // ---- bend-range rails, top ----
        static const char *const kRailLbl[2] = { "BEND UP (st)", "BEND DOWN (st)" };
        const float rval[2] = { s.tuneUp, s.tuneDn };
        const float rrow[2] = { tune::railY, tune::railY2 };
        for (int k = 0; k < 2; k++)
        {
            nvgTextAlign(g.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgFontSize(g.vg, 12.f);
            nvgFillColor(g.vg, col(kColInk));
            nvgText(g.vg, sxp(tune::railX - 12), syp(rrow[k]), kRailLbl[k], nullptr);
            nvgBeginPath(g.vg);
            nvgRect(g.vg, tune::railX, rrow[k] - 1.5f, tune::railW, 3);
            nvgFillColor(g.vg, nvgRGBA(70, 70, 78, 255));
            nvgFill(g.vg);
            for (int q = 0; q <= 24; q += 2)
            {
                nvgBeginPath(g.vg);
                nvgRect(g.vg, tune::railX + (q / 24.f) * tune::railW - 0.5f,
                        rrow[k] + (q % 12 ? 4 : 6), 1.f, q % 12 ? 3.f : 4.f);
                nvgFillColor(g.vg, nvgRGBA(120, 120, 130, 255));
                nvgFill(g.vg);
            }
            nvgBeginPath(g.vg);
            nvgCircle(g.vg, tune::railHandleX(k, rval[k]), rrow[k], 6);
            nvgFillColor(g.vg, nvgRGBA(240, 160, 30, 255));
            nvgFill(g.vg);
            if (k == s.tuneFocus)
            {   nvgBeginPath(g.vg);
                nvgCircle(g.vg, tune::railHandleX(k, rval[k]), rrow[k], 9.f);
                nvgStrokeColor(g.vg, nvgRGBA(240, 240, 246, 220));
                nvgStrokeWidth(g.vg, 1.4f);
                nvgStroke(g.vg);
            }
            char vt[10];
            std::snprintf(vt, sizeof vt, "%.1f", rval[k]);
            nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFontSize(g.vg, 13.f);
            nvgFillColor(g.vg, col(kColInk));
            nvgText(g.vg, sxp(tune::railX + tune::railW + 10), syp(rrow[k]), vt, nullptr);
        }

        // ---- root spinner, top right ----
        nvgTextAlign(g.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFontSize(g.vg, 12.f);
        nvgText(g.vg, sxp(tune::spinMinusX - 18), syp(tune::spinY), "ROOT", nullptr);
        for (int k = 0; k < 2; k++)
        {
            const float bx = k ? tune::spinPlusX : tune::spinMinusX;
            nvgBeginPath(g.vg);
            nvgRoundedRect(g.vg, bx - 12, tune::spinY - 11, 24, 22, 3);
            nvgFillColor(g.vg, nvgRGBA(52, 52, 58, 255));
            nvgFill(g.vg);
            nvgStrokeColor(g.vg, col(kColBox));
            nvgStrokeWidth(g.vg, 1.f);
            nvgStroke(g.vg);
            nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFontSize(g.vg, 16.f);
            nvgFillColor(g.vg, col(kColInk));
            nvgText(g.vg, sxp(bx), syp(tune::spinY + 1), k ? "+" : "-", nullptr);
        }
        {   static const char *const kNote[12] = { "C", "C#", "D", "D#", "E", "F",
                                                   "F#", "G", "G#", "A", "A#", "B" };
            char rt[16];
            std::snprintf(rt, sizeof rt, "%s  (%d)", kNote[((s.tuneRoot % 12) + 12) % 12], s.tuneRoot);
            nvgBeginPath(g.vg);
            nvgRoundedRect(g.vg, tune::spinBoxX, tune::spinY - 11, tune::spinBoxW, 22, 3);
            nvgFillColor(g.vg, col(kColDispBg));
            nvgFill(g.vg);
            nvgStrokeColor(g.vg, col(kColDispStroke));
            nvgStrokeWidth(g.vg, 1.f);
            nvgStroke(g.vg);
            nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFontSize(g.vg, 14.f);
            nvgFillColor(g.vg, col(kColDispText));
            nvgText(g.vg, sxp(tune::spinBoxX + tune::spinBoxW / 2), syp(tune::spinY), rt, nullptr);
        }

        // ---- polar dial, bottom left ----
        {
            float wc[64]; int wn = 0;
            if (s.wheelN >= 2 && s.wheelN <= 64 && s.wheelC)
            {   wn = s.wheelN; for (int i = 0; i < wn; i++) wc[i] = s.wheelC[i]; }
            else { wn = 12; for (int i = 0; i < wn; i++) wc[i] = 100.f * (i + 1); }
            const bool grid = s.wheelN < 2;
            const float et = tune::etCents(wc, wn);
            const float rs = tune::ringScale(wc, wn);
            nvgBeginPath(g.vg);
            nvgCircle(g.vg, tune::wheelCX, tune::wheelCY, tune::wheelR);
            nvgStrokeColor(g.vg, nvgRGBA(130, 130, 140, 220));
            nvgStrokeWidth(g.vg, 1.2f);
            nvgStroke(g.vg);
            for (int q = -6; q <= 3; q++)
            {   if (!q) continue;
                const float rad = tune::wheelR + q * rs;
                if (rad < 10.f || rad > tune::wheelR * 1.42f) continue;
                nvgBeginPath(g.vg);
                nvgCircle(g.vg, tune::wheelCX, tune::wheelCY, rad);
                nvgStrokeColor(g.vg, nvgRGBA(85, 85, 95, grid ? 80 : 150));
                nvgStrokeWidth(g.vg, 1.f);
                nvgStroke(g.vg);
            }
            const float rOut = tune::wheelR * 1.16f;
            for (int k = 0; k < wn; k++)
            {
                const float a = -1.5707963f + (float)k * (6.2831853f / (float)wn);
                nvgBeginPath(g.vg);
                nvgMoveTo(g.vg, tune::wheelCX, tune::wheelCY);
                nvgLineTo(g.vg, tune::wheelCX + rOut * std::cos(a), tune::wheelCY + rOut * std::sin(a));
                nvgStrokeColor(g.vg, nvgRGBA(110, 110, 120, grid ? 90 : 200));
                nvgStrokeWidth(g.vg, 1.f);
                nvgStroke(g.vg);
                if (wn <= 24)
                {   char lab[4]; std::snprintf(lab, sizeof lab, "%d", k);
                    nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nvgFontSize(g.vg, 10.f);
                    nvgFillColor(g.vg, col(kColInk));
                    nvgText(g.vg, sxp(tune::wheelCX + (rOut + 8.f) * std::cos(a)),
                            syp(tune::wheelCY + (rOut + 8.f) * std::sin(a)), lab, nullptr); }
            }
            for (int i = 1; i <= wn; i++)
            {
                float x, y; tune::nodePos(wc, wn, i, x, y);
                if (wn <= 24)
                {   const float a = -1.5707963f + (float)(i - 1) * (6.2831853f / (float)wn);
                    const float ri = tune::wheelR - 20.f;
                    char iv[8]; std::snprintf(iv, sizeof iv, "%.0f", wc[i - 1] - (i > 1 ? wc[i - 2] : 0.f));
                    nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    nvgFontSize(g.vg, 10.f);
                    nvgFillColor(g.vg, nvgRGBA(200, 200, 206, 255));
                    nvgText(g.vg, sxp(tune::wheelCX + ri * std::cos(a)),
                            syp(tune::wheelCY + ri * std::sin(a)), iv, nullptr);
                }
                const float dev = (wc[i - 1] - et * i) / et;
                nvgBeginPath(g.vg);
                nvgCircle(g.vg, x, y, i == s.wheelHot ? 6.5f : 5.f);
                nvgFillColor(g.vg, dev < -0.02f ? nvgRGBA(120, 150, 226, 255) :
                                 dev >  0.02f ? nvgRGBA(232, 200, 96, 255) :
                                                nvgRGBA(200, 200, 206, 255));
                nvgFill(g.vg);
                if (i == s.wheelHot)
                {   nvgBeginPath(g.vg);
                    nvgCircle(g.vg, x, y, 9.5f);
                    nvgStrokeColor(g.vg, nvgRGBA(240, 240, 246, 230));
                    nvgStrokeWidth(g.vg, 1.5f);
                    nvgStroke(g.vg);
                }
            }
            if (grid)
            {   nvgFontSize(g.vg, 10.f);
                nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgFillColor(g.vg, nvgRGBA(150, 150, 158, 255));
                nvgText(g.vg, sxp(tune::wheelCX), syp(tune::wheelCY + tune::wheelR * 1.16f + 20.f),
                        "12-TET grid - drag a node to edit", nullptr);
            }
        }

        // ---- chromatic text panel, bottom right ----
        {
            nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFontSize(g.vg, 10.f);
            nvgFillColor(g.vg, nvgRGBA(150, 150, 158, 255));
            nvgText(g.vg, sxp(tune::tblX), syp(tune::tblTop - 16), "KEY", nullptr);
            nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(g.vg, sxp(tune::tblX + 150), syp(tune::tblTop - 16), "FREQUENCY", nullptr);
            nvgText(g.vg, sxp(tune::tblX + 330), syp(tune::tblTop - 16), "CENTS vs 12-TET", nullptr);
            static const char *const kNote[12] = { "C", "C#", "D", "D#", "E", "F",
                                                   "F#", "G", "G#", "A", "A#", "B" };
            nvgFontSize(g.vg, 11.f);
            for (int i = 0; i < tune::rows; i++)
            {
                const int key = 48 + i;
                const float ry = tune::tblTop + i * tune::tblRowH;
                const int dev = s.tuneDev[key];
                const double f = 440.0 * std::pow(2.0, (key - 69 + dev / 16.0) / 12.0);
                char lab[8]; std::snprintf(lab, sizeof lab, "%s3", kNote[i]);
                nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgFillColor(g.vg, col(kColInk));
                nvgText(g.vg, sxp(tune::tblX), syp(ry), lab, nullptr);
                char ft[16]; std::snprintf(ft, sizeof ft, "%.2f Hz", f);
                nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgText(g.vg, sxp(tune::tblX + 150), syp(ry), ft, nullptr);
                char ct[12]; std::snprintf(ct, sizeof ct, dev ? "%+.1f" : "%.1f", dev * 6.25f);
                nvgFillColor(g.vg, dev ? nvgRGBA(240, 160, 30, 255) : col(kColInk));
                nvgText(g.vg, sxp(tune::tblX + 330), syp(ry), ct, nullptr);
                nvgFillColor(g.vg, col(kColInk));
            }
        }

        // ---- LOAD / SAVE / RESET ----
        for (int k = 0; k < 3; k++)
        {
            const float bx = k == 0 ? tune::loadX : k == 1 ? tune::saveX : tune::resetX;
            const char *lbl = k == 0 ? "LOAD" : k == 1 ? "SAVE" : "RESET";
            nvgBeginPath(g.vg);
            nvgRoundedRect(g.vg, bx, tune::btnY, tune::loadW, tune::btnH, 3);
            nvgFillColor(g.vg, nvgRGBA(52, 52, 58, 255));
            nvgFill(g.vg);
            nvgStrokeColor(g.vg, col(kColBox));
            nvgStrokeWidth(g.vg, 1.f);
            nvgStroke(g.vg);
            nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFontSize(g.vg, 13.f);
            nvgFillColor(g.vg, col(kColInk));
            nvgText(g.vg, sxp(bx + tune::loadW / 2), syp(tune::btnY + tune::btnH / 2), lbl, nullptr);
        }
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFontSize(g.vg, 10.f);
        nvgFillColor(g.vg, col(kColInk));
        nvgText(g.vg, sxp(tune::x + 14), syp(tune::y + tune::h - 12),
                "drag nodes to edit - dbl-click snaps a tone to equal  -  arrows on ringed rail", nullptr);
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    }

    // ---- tuning file picker (same sheet geometry as the patch picker) ----
    if (s.tunePick)
    {
        nvgBeginPath(g.vg);
        nvgRect(g.vg, 0, 0, kPanelW, kPanelH);
        nvgFillColor(g.vg, nvgRGBA(0, 0, 0, 170));
        nvgFill(g.vg);
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, pick::x, pick::y, pick::w, pick::h, 6);
        nvgFillColor(g.vg, col(kColPanel));
        nvgFill(g.vg);
        nvgStrokeColor(g.vg, col(kColBox));
        nvgStrokeWidth(g.vg, 1.5f);
        nvgStroke(g.vg);
        nvgFontFaceId(g.vg, g.fontId);
        for (int tt = 0; tt < 2; tt++)          // STOCK | USER tabs (patch-picker geometry)
        {
            const float tx = pick::x + 12 + tt * 150;
            nvgBeginPath(g.vg);
            nvgRoundedRect(g.vg, tx, pick::y + 10, 144, 30, 3);
            nvgFillColor(g.vg, s.tuneTab == tt ? col(kColDispBg) : nvgRGBA(52, 52, 58, 255));
            nvgFill(g.vg);
            nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFontSize(g.vg, 14.f);
            nvgFillColor(g.vg, s.tuneTab == tt ? col(kColDispText) : col(kColInk));
            nvgText(g.vg, sxp(tx + 72), syp(pick::y + 25), tt ? "USER" : "STOCK", nullptr);
        }
        {   const char *pp[2] = { "<", ">" };          // page controls (hitPage bands)
            for (int pt = 0; pt < 2; pt++)
            {
                const float px2 = pick::x + pick::w - 172 + pt * 50;
                nvgBeginPath(g.vg);
                nvgRoundedRect(g.vg, px2, pick::y + 10, 44, 26, 3);
                nvgFillColor(g.vg, nvgRGBA(52, 52, 58, 255));
                nvgFill(g.vg);
                nvgStrokeColor(g.vg, col(kColDispStroke));
                nvgStroke(g.vg);
                nvgFontSize(g.vg, 11.f);
                nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgFillColor(g.vg, col(kColInk));
                nvgText(g.vg, sxp(px2 + 22), syp(pick::y + 23), pp[pt], nullptr);
            }
            char pg[16];
            std::snprintf(pg, sizeof pg, "%d/%d", s.tunePage + 1, s.tunePages < 1 ? 1 : s.tunePages);
            nvgTextAlign(g.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgFontSize(g.vg, 14.f);
            nvgFillColor(g.vg, col(kColInk));
            nvgText(g.vg, sxp(pick::x + pick::w - 182), syp(pick::y + 23), pg, nullptr);
            nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(g.vg, sxp(pick::x + 320), syp(pick::y + 23), "LOAD TUNING", nullptr);
        }
        nvgFontSize(g.vg, 15.f);
        for (int i = 0; i < pick::rows; i++)
        {
            if (!s.tuneRows[i]) continue;
            const float ry = pick::rowTop(i);
            if (i == s.tuneSel)
            {
                nvgBeginPath(g.vg);
                nvgRoundedRect(g.vg, pick::x + 10, ry - pick::rowH / 2 + 2, pick::w - 20, pick::rowH - 4, 3);
                nvgFillColor(g.vg, col(kColDispBg));
                nvgFill(g.vg);
                nvgFillColor(g.vg, col(kColDispText));
            }
            else nvgFillColor(g.vg, col(kColInk));
            nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(g.vg, sxp(pick::x + 22), syp(ry), s.tuneRows[i], nullptr);
        }
        nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(g.vg, 13.f);
        nvgFillColor(g.vg, col(kColInk));
        char pg[16];
        std::snprintf(pg, sizeof pg, "%d / %d", s.tunePage + 1, s.tunePages);
        nvgText(g.vg, sxp(pick::x + pick::w / 2), syp(pick::y + pick::h - 18), pg, nullptr);
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    }

    // ---- full debug info page (modal 10; click status strip to open) ----
    if (s.debugOpen && s.debugText && s.debugText[0])
    {
        nvgBeginPath(g.vg);
        nvgRect(g.vg, -xoff / sc, -yoff / sc, (w - xoff) / sc, (h - yoff) / sc);  // whole window
        nvgFillColor(g.vg, nvgRGBA(0, 0, 0, 190));
        nvgFill(g.vg);
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, dbgpg::x, dbgpg::y, dbgpg::w, dbgpg::h, 6);
        nvgFillColor(g.vg, col(kColPanel));
        nvgFill(g.vg);
        nvgStrokeColor(g.vg, col(kColBox));
        nvgStrokeWidth(g.vg, 1.5f);
        nvgStroke(g.vg);
        nvgFontFaceId(g.vg, g.fontId);
        nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(g.vg, 16.f);
        nvgFillColor(g.vg, col(kColInk));
        nvgText(g.vg, sxp(dbgpg::x + dbgpg::w / 2), syp(dbgpg::y + 24), "DEBUG INFO", nullptr);
        // close X chip
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, dbgpg::x + dbgpg::w - 44, dbgpg::y + 10, 30, 26, 3);
        nvgFillColor(g.vg, nvgRGBA(90, 40, 36, 255));
        nvgFill(g.vg);
        nvgFontSize(g.vg, 12.f);
        nvgFillColor(g.vg, col(kColInk));
        nvgText(g.vg, sxp(dbgpg::x + dbgpg::w - 29), syp(dbgpg::y + 23), "X", nullptr);
        // body text: screen font, pre-wrapped lines
        nvgFontFaceId(g.vg, g.screenId);
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFontSize(g.vg, 15.5f);
        nvgFillColor(g.vg, col(kColInk));
        {   const char *p = s.debugText;
            float ty = dbgpg::y + 48;
            while (*p && ty < dbgpg::y + 352)
            {   const char *e = std::strchr(p, '\n');
                const int len = e ? (int)(e - p) : (int)std::strlen(p);
                char rowl[256];
                if (len > 0 && len < (int)sizeof rowl)
                {   std::memcpy(rowl, p, (size_t)len); rowl[len] = 0;
                    nvgText(g.vg, sxp(dbgpg::x + 18), syp(ty), rowl, nullptr); }
                ty += 22.f;
                if (!e) break;
                p = e + 1;
            }
        }
        // COPY button
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, dbgpg::x + dbgpg::copyX - dbgpg::x, dbgpg::copyY, dbgpg::copyW, dbgpg::copyH, 3);
        nvgFillColor(g.vg, nvgRGBA(52, 52, 58, 255));
        nvgFill(g.vg);
        nvgStrokeColor(g.vg, col(kColBox));
        nvgStrokeWidth(g.vg, 1.f);
        nvgStroke(g.vg);
        nvgFontFaceId(g.vg, g.fontId);
        nvgTextAlign(g.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(g.vg, 13.f);
        nvgFillColor(g.vg, col(kColInk));
        nvgText(g.vg, sxp(dbgpg::copyX + dbgpg::copyW / 2), syp(dbgpg::copyY + dbgpg::copyH / 2),
                "COPY", nullptr);
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFontSize(g.vg, 10.f);
        nvgFillColor(g.vg, col(kColInk));
        nvgText(g.vg, sxp(dbgpg::copyX + 150), syp(dbgpg::copyY + 13),
                "copies everything above - click anywhere else to close", nullptr);
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    }

    // ---- text-entry modal over the programmer area (RECORD save / PRESET assign) ----
    if (s.modalTitle && s.modalTitle[0])
    {
        const float bx = 600, by = 292, bw = 580, bh = 124;
        nvgBeginPath(g.vg);
        nvgRect(g.vg, 0, 0, kPanelW, kPanelH);
        nvgFillColor(g.vg, nvgRGBA(0, 0, 0, 150));          // dim the panel behind
        nvgFill(g.vg);
        nvgBeginPath(g.vg);
        nvgRoundedRect(g.vg, bx, by, bw, bh, 6);
        nvgFillColor(g.vg, col(kColPanel));
        nvgFill(g.vg);
        nvgStrokeColor(g.vg, col(kColBox));
        nvgStrokeWidth(g.vg, 1.5f);
        nvgStroke(g.vg);
        nvgTextAlign(g.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        nvgFontSize(g.vg, kTxtSize[T_HDR]);
        nvgFillColor(g.vg, col(kColInk));
        nvgText(g.vg, bx + 14, by + 22, s.modalTitle, nullptr);
        nvgFontSize(g.vg, 8.5f);
        nvgFillColor(g.vg, nvgRGBA(180, 180, 190, 255));
        nvgText(g.vg, bx + 14, by + 40, s.modalPrompt ? s.modalPrompt : "", nullptr);
        nvgBeginPath(g.vg);                                   // text field
        nvgRoundedRect(g.vg, bx + 14, by + 52, bw - 28, 34, 3);
        nvgFillColor(g.vg, col(kColDispBg));
        nvgFill(g.vg);
        nvgStrokeColor(g.vg, col(kColDispStroke));
        nvgStrokeWidth(g.vg, 1.f);
        nvgStroke(g.vg);
        nvgFontFaceId(g.vg, g.screenId);
        nvgFontSize(g.vg, 18.f);
        nvgFillColor(g.vg, col(kColDispText));
        char shown[32];
        std::snprintf(shown, sizeof shown, "%.*s", s.modalLen, s.modalText ? s.modalText : "");
        nvgText(g.vg, sxp(bx + 22), syp(by + 76), shown, nullptr);
        const float cw = nvgTextBounds(g.vg, 0, 0, shown, nullptr, nullptr);
        nvgBeginPath(g.vg);
        nvgRect(g.vg, bx + 23 + cw, by + 62, 7, 18);          // block cursor
        nvgFillColor(g.vg, nvgRGBA(255, 90, 60, 200));
        nvgFill(g.vg);
        nvgFontFaceId(g.vg, g.fontId);
        nvgFontSize(g.vg, 8.0f);
        nvgFillColor(g.vg, nvgRGBA(180, 180, 190, 255));
        nvgText(g.vg, bx + 14, by + 112, "ENTER confirm   ESC cancel   BS delete   upper+lower case kept", nullptr);
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

void pointToLayout(float px, float py, float w, float h, float &lx, float &ly)
{ toLayout(w, h, px, py, lx, ly); }

namespace pick {
int hitTab(float lx, float ly)
{
    if (ly < y + 10 || ly > y + 40) return -1;
    if (lx >= x + 12 && lx <= x + 156) return 0;
    if (lx >= x + 162 && lx <= x + 306) return 1;
    return -1;
}
int hitRow(float lx, float ly)
{
    if (lx < x + 12 || lx > x + w - 12 - assignW()) return -1;    // body only, not the chip
    for (int i = 0; i < rows; i++)
        if (ly >= rowTop(i) && ly <= rowTop(i) + rowH) return i;
    return -1;
}
int hitAssign(float lx, float ly)
{
    if (lx < x + w - 12 - assignW() || lx > x + w - 12) return -1;
    for (int i = 0; i < rows; i++)
        if (ly >= rowTop(i) && ly <= rowTop(i) + rowH) return i;
    return -1;
}
int hitPage(float lx, float ly)
{
    if (ly < y + 10 || ly > y + 36) return 0;
    if (lx >= x + w - 172 && lx <= x + w - 128) return -1;
    if (lx >= x + w - 122  && lx <= x + w - 78)  return +1;
    return 0;
}
bool hitClose(float lx, float ly)
{ return lx >= x + w - 44 && lx <= x + w - 14 && ly >= y + 10 && ly <= y + 36; }
bool inside(float lx, float ly)
{ return lx >= x && lx <= x + w && ly >= y && ly <= y + h; }
} // namespace pick

namespace trim {
bool inside(float lx, float ly)
{   return lx >= x && lx <= x + w && ly >= y && ly <= y + h; }
int hitBar(float lx, float ly, int *row, int *col)
{
    if (!inside(lx, ly)) return 0;
    for (int i = 0; i < rows; i++)
    {
        const float t = rowTop(i);
        if (ly >= t - 10.f && ly <= t + 30.f)
        {
            *row = i;
            for (int k = 0; k < ncols; k++)
                if (lx >= barX(k) - 10.f && lx <= barX(k) + barW(k) + 10.f) { *col = k; return 1; }
            *col = -1; return 1;
        }
    }
    return 0;
}
} // namespace trim

namespace dbgpg {
bool hitCopy(float lx, float ly)
{   return lx >= x && lx <= x + w && ly >= y && ly <= y + h &&
         lx >= copyX && lx <= copyX + copyW && ly >= copyY && ly <= copyY + copyH; }
bool hitClose(float lx, float ly)
{   return lx >= x && lx <= x + w && ly >= y && ly <= y + h; }
}

namespace tune {
float railValAt(int k, float lx)
{   float t = (lx - railX) / railW; t = t < 0.f ? 0.f : t > 1.f ? 1.f : t; return t * 24.f; }
bool inside(float lx, float ly) { return lx >= x && ly >= y && lx <= x + w && ly <= y + h; }
int hitControl(float lx, float ly)
{
    if (ly >= btnY - 2 && ly <= btnY + btnH + 2)
    {
        if (lx >= loadX && lx <= loadX + loadW) return 1;
        if (lx >= saveX && lx <= saveX + saveW) return 5;
        if (lx >= resetX && lx <= resetX + resetW) return 2;
    }
    if (ly >= spinY - 12 && ly <= spinY + 12)
    {
        if (lx >= spinMinusX - 13 && lx <= spinMinusX + 13) return 6;
        if (lx >= spinPlusX - 13 && lx <= spinPlusX + 13) return 7;
    }
    if (lx >= railX - 10 && lx <= railX + railW + 44)
    {
        if (ly >= railY - 12 && ly <= railY + 12) return 3;
        if (ly >= railY2 - 12 && ly <= railY2 + 12) return 4;
    }
    return 0;
}
float etCents(const float *c, int n) { return n > 0 ? c[n - 1] / n : 100.f; }
float ringScale(const float *c, int n)
{   float mx = 0.2f;
    if (n >= 2)
    {   const float et = etCents(c, n);
        for (int i = 1; i <= n; i++) { float d = std::fabs(c[i - 1] - et * i) / et; if (d > mx) mx = d; }
    }
    const float range = std::max(0.2f, std::ceil(mx * 5.f) / 5.f);   // 0.2-ET floor
    return 0.6f * wheelR / range;
}
static float nodeRadiusPx(const float *c, int n, int i)
{   const float et = etCents(c, n);
    return wheelR + ((c[i - 1] - et * i) / et) * ringScale(c, n); }
void nodePos(const float *c, int n, int i, float &ox, float &oy)
{   const float a = -1.5707963f + (float)(i - 1) * (6.2831853f / (float)n);
    const float r = nodeRadiusPx(c, n, i);
    ox = wheelCX + r * std::cos(a); oy = wheelCY + r * std::sin(a); }
int nodeAt(const float *c, int n, float lx, float ly)
{   float best = 10.f; int idx = -1;
    for (int i = 1; i <= n; i++)
    {   float px, py; nodePos(c, n, i, px, py);
        const float d = std::hypot(lx - px, ly - py);
        if (d < best) { best = d; idx = i; }
    }
    return idx; }
float clampTone(const float *c, int n, int i, float v)
{   const float lo = (i > 1 ? c[i - 2] : 0.f) + 1.f;
    const float hi = (i < n ? c[i] : 1e9f) - 1.f;
    if (v < lo) v = lo; if (v > hi) v = hi;
    return v; }
} // namespace tune



} // namespace p5ui
