// SPDX-License-Identifier: GPL-3.0-only
// Platform-neutral GUI core implementation. Lifted out of gui_x11.cpp:
// struct Ctx's neutral fields became GuiCore (gui_core.hpp); every function
// here keeps the original semantics exactly. The only substitutions are
// X11-specific side effects the shims now own:
//   XSetInputFocus  -> c.needFocus = true (consumed by the shim)
//   XGrabPointer    -> the shim grabs whenever a core drag field armed
//                      (dragKnob/trimDrag/tuneDrag/wheelDragIdx) after a press
//   ev.xbutton.time -> the timeMs parameter of onPointerPress
//   ShiftMask       -> P5_MOD_SHIFT (same bit value: 1)
//   1u << 30        -> P5_MOD_RIGHT (same sentinel actuate() already used)
#include "gui_core.hpp"

#include <nanovg.h>

#include <clap/ext/timer-support.h>

#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <unistd.h>
#include <cstdio>
#include <cstring>

#include "muhammad_plugin.hpp"
#include "p5_common.hpp"
#include "p5_stock_scales.hpp"

namespace pw {

namespace {

void applyTrimSet(GuiCore &c, int row, int col, int v)
{   // main-thread: engine trim setters are POD writes; trims apply at the
    // oscillator increment read, so held notes keep re-trimming live
    const int osc = col / 2, which = col % 2;
    int sa, oa, sb, ob;
    c.plugin->engine.voiceTrim(row, 0, &sa, &oa);
    c.plugin->engine.voiceTrim(row, 1, &sb, &ob);
    if (which == 0) { if (osc == 0) sa = v; else sb = v; }
    else            { if (osc == 0) oa = v; else ob = v; }
    c.plugin->engine.setVoiceTrim(row, 0, sa, oa);
    c.plugin->engine.setVoiceTrim(row, 1, sb, ob);
}

void applyTrimDrag(GuiCore &c, float lx)
{   applyTrimSet(c, c.trimRow, c.trimCol, p5ui::trim::valueAt(c.trimCol, lx));
}

void wheelSeed(GuiCore &c);

void wheelApply(GuiCore &c);

void wheelSeed(GuiCore &c)
{   const Plugin *P = c.plugin;
    c.wheelN = (P->tuneToneCount_ >= 2 && P->tuneToneCount_ <= 64) ? P->tuneToneCount_ : 0;
    for (int i = 0; i < c.wheelN; i++) c.wheelC[i] = (float)P->tuneTones_[i];
    c.wheelMask = P->tuneMask_; c.wheelWalk = P->tuneWalk_; c.wheelRoot = P->tuneRoot_;
}

void applyStock(GuiCore &c, int si)
{   const StockScale &sc = P5_STOCK[si];
    c.wheelN = sc.degrees;
    for (int i = 0; i < sc.degrees; i++) c.wheelC[i] = sc.p[i + 1];   // drop unison, keep period
    c.wheelMask = sc.mask; c.wheelWalk = sc.walk;
    wheelApply(c);
    std::snprintf(c.plugin->tuneName_, sizeof c.plugin->tuneName_, "%s", sc.title);
    std::fprintf(stderr, "[p5gui] stock scale: %s (%d tones, mask %03x, %s)\n",
                 sc.title, sc.degrees, sc.mask, sc.walk ? "ALL-walk" : "quantized");
}

void wheelApply(GuiCore &c)
{   if (c.wheelN < 2) return;
    double p[66]; p[0] = 0.0;
    for (int i = 0; i < c.wheelN; i++) p[i + 1] = c.wheelC[i];
    int16_t dev[128];
    if (!p5store::buildScaleDev(p, c.wheelN, c.wheelMask, c.wheelRoot, dev)) return;
    c.plugin->engine.setTuningDeviation(dev);
    c.plugin->tuneToneCount_ = c.wheelN;
    c.plugin->tuneMask_ = c.wheelMask; c.plugin->tuneWalk_ = c.wheelWalk;
    c.plugin->tuneRoot_ = c.wheelRoot;
    for (int i = 0; i < c.wheelN; i++) c.plugin->tuneTones_[i] = c.wheelC[i];
}

void wheelCommit(GuiCore &c)         // user edited: apply + mark the name
{   wheelApply(c);
    char *n = c.plugin->tuneName_;
    if (!std::strchr(n, '*') && std::strlen(n) + 3 < sizeof c.plugin->tuneName_)
        std::strncat(n, " *", sizeof c.plugin->tuneName_ - std::strlen(n) - 1);
}

void applyWheelDrag(GuiCore &c, float lx, float ly, unsigned mods)
{   const int i = c.wheelDragIdx;
    if (i < 1 || i > c.wheelN) return;
    const float dist = std::hypot(lx - p5ui::tune::wheelCX, ly - p5ui::tune::wheelCY);
    const float et = p5ui::tune::etCents(c.wheelC, c.wheelN);
    float dc = (dist - c.wheelDragDist0) / p5ui::tune::ringScale(c.wheelC, c.wheelN) * et;
    if (mods & P5_MOD_SHIFT) dc *= 0.1f;                       // fine edit (Surge's shift speed)
    const float nc = p5ui::tune::clampTone(c.wheelC, c.wheelN, i, c.wheelDragC0 + dc);
    if (nc != c.wheelC[i - 1]) { c.wheelC[i - 1] = nc; wheelCommit(c); }
}

void applyTuneDrag(GuiCore &c, float lx, unsigned mods)
{   const int k = c.tuneDrag - 3;
    float v = p5ui::tune::railValAt(k, lx);
    if (mods & P5_MOD_SHIFT) v = (float)(int)(v + 0.5f);   // Shift: snap to integer semitones
    const float up = k == 0 ? v : (float)c.plugin->engine.bendRangeUpDefault();
    const float dn = k == 1 ? v : (float)c.plugin->engine.bendRangeDownDefault();
    c.plugin->engine.setBendRangeDefault(up, dn);   // resolved pair stored (WYSIWYG)
}

// param value from a normalized knob position, in the param's own units
double normToValue(int pid, double norm)
{
    const p5::KnobRange *r = p5::knobRange(pid);
    double lo = r ? r->lo : 0.0, hi = r ? r->hi : (pid < 55 ? 127.0 : 255.0);
    return lo + norm * (hi - lo);
}

} // namespace

// ---- state <-> layout -------------------------------------------------------

void fillState(GuiCore &c, p5ui::State &st)
{
    c.plugin->adoptPendingIdentity();
    st.trimOpen = (c.modal == 5);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 2; k++)
        {   int sc, of; c.plugin->engine.voiceTrim(i, k, &sc, &of);
            st.trimV[4*i+2*k] = (short)sc; st.trimV[4*i+2*k+1] = (short)of; }     // audio-thread PC applied bytes; pull identity here
    st.tuneOpen = (c.modal == 6);
    st.tunePick = (c.modal == 7);
    st.tuneTab = c.tuneTab;
    std::snprintf(st.tuneName, sizeof st.tuneName, "%s", c.plugin->tuneName_);
    for (int k = 0; k < 128; k++) st.tuneDev[k] = c.plugin->engine.tuningDeviation()[k];
    st.tuneUp = (float)c.plugin->engine.bendRangeUpDefault();
    st.tuneDn = (float)c.plugin->engine.bendRangeDownDefault();
    st.tuneFocus = c.tuneFocus;
    st.wheelC = c.wheelC; st.wheelN = c.wheelN; st.wheelHot = c.wheelHot;
    st.tuneRoot = c.wheelRoot;
    {
        const int per = p5ui::pick::rows;
        const int total = 1 + (c.tuneTab ? (int)c.tuneFiles.size() : P5_STOCK_N);
        st.tunePages = (total + per - 1) / per;
        if (c.tunePage >= st.tunePages) c.tunePage = st.tunePages - 1;
        if (c.tunePage < 0) c.tunePage = 0;
        st.tunePage = c.tunePage;
        int sel = 0;                                  // row index of the loaded tuning (0 = standard)
        const std::string nm(c.plugin->tuneName_);
        if (c.tuneTab == 0)
        {   for (int i = 0; i < P5_STOCK_N; i++) if (nm == P5_STOCK[i].title) sel = 1 + i; }
        else for (size_t i = 0; i < c.tuneFiles.size(); i++) if (c.tuneFiles[i] == nm) sel = 1 + (int)i;
        st.tuneSel = sel - c.tunePage * per;
        if (st.tuneSel < 0 || st.tuneSel >= per) st.tuneSel = -1;
        for (int i = 0; i < per; i++)
        {
            const int idx = c.tunePage * per + i;
            if (idx >= total) { st.tuneRows[i] = nullptr; continue; }
            if (idx == 0) std::snprintf(c.tuneRowBuf[i], 64, "12-TET (standard)");
            else if (c.tuneTab == 0)
                std::snprintf(c.tuneRowBuf[i], 64, "%s  -  %s", P5_STOCK[idx - 1].group, P5_STOCK[idx - 1].title);
            else
            {   std::string s2 = c.tuneFiles[idx - 1];
                const size_t dot = s2.rfind('.');
                if (dot != std::string::npos) s2 = s2.substr(0, dot);   // stem, not file, is the identity
                std::snprintf(c.tuneRowBuf[i], 64, "%s", s2.c_str());
            }
            st.tuneRows[i] = c.tuneRowBuf[i];
        }
    }
    for (int i = 0; i < p5ui::kNumKnobs; i++)
        st.knob[i] = p5::knobNorm(p5ui::kKnobs[i].pid, c.plugin->guiValue(p5ui::kKnobs[i].pid));
    const int program = c.plugin->program, bank = c.plugin->bank;
    for (int i = 0; i < p5ui::kNumBtns; i++)
    {
        const auto &b = p5ui::kBtns[i];
        if (b.pid == -1)                              // EDIT button LED: lit while page open
        {   st.led[i] = c.modal == 5 ? 2 : 0; continue; }
        if (b.pid == -6)                              // TUNING button LED
        {   st.led[i] = (c.modal == 6 || c.modal == 7) ? 2 : 0; continue; }
        if (b.ledmode == 1) st.led[i] = c.plugin->guiValue(b.pid) != 0.0 ? 2 : 0;
        else if (b.ledmode == 2)
        {   // light the quickload button whose SLOT this patch is bound to
            int bs = -1;
            st.led[i] = (p5store::findSlotFor(c.plugin->curSrc_, bs) && bs == bank * 8 + b.value) ? 2 : 0;
        }
        else if (b.ledmode == 3) st.led[i] = c.plugin->panelDirty() ? 2 : 0;   // PRESET: floating
        else if (b.ledmode == 4) st.led[i] = (c.modal == 1 || c.modal == 3) ? 2 : 0;  // RECORD armed
        else if (b.ledmode == 5) st.led[i] = (c.modal == 2 || c.modal == 4) ? 2 : 0;  // PRESET picker
        else if (b.ledmode == 6)                                          // KEYBD: OFF/HALF/FULL
        {
            const double v = c.plugin->guiValue(b.pid);
            st.led[i] = v > 1.5 ? 2 : v > 0.5 ? 1 : 0;                     // red / amber / off
        }
    }
    st.debugOpen = (c.modal == 10);
    st.debugText = c.modal == 10 ? c.dbg : nullptr;
#ifdef P5_BUILD_INFO
    st.build = P5_BUILD_INFO;
#endif
    {   // live diagnostics in-window: Bitwig's plugin host swallows our stderr
        static char diagBuf[140];
        std::snprintf(diagBuf, sizeof diagBuf, "svc %s rr %d src %s simd %s win %ux%u p %ux%u a %ux%u",
                      c.hostGui ? "y" : "n", c.resizeRequests, c.plugin->loadTag_,
                      c.plugin->engine.simdPath(),
                      c.w, c.h, c.parW, c.parH, c.ancW, c.ancH);
        st.diag = diagBuf;
    }
    std::memcpy(st.name, c.plugin->engine.patch().raw + p5::P5_NAME, 20);
    st.name[20] = 0;
    for (char *q = st.name + 19; q >= st.name && *q == ' '; *q-- = 0) {}
    if (c.modal == 4)
    {
        static char title[96];
        std::snprintf(title, sizeof title, "row = load   ASSIGN then 1-8");
        st.pickerTitle = title;
        st.pickerTab = c.pickerTab; st.pickerPage = c.pickerPage;
        const int total = c.pickerTab == 0 ? (int)PW_FACTORY_N + 2 : (int)c.userRows.size();
        st.pickerPages = total ? (total + p5ui::pick::rows - 1) / p5ui::pick::rows : 1;
        st.pickerCount = 0;
        for (int i = 0; i < p5ui::pick::rows; i++)
        {
            const int g = c.pickerPage * p5ui::pick::rows + i;
            if (g >= total) break;
            if (c.pickerTab == 0)
            {
                if (g == 0) std::snprintf(st.pickerRows[i], 44, "  INIT - Peace Be Upon Him (default)");
                else if (g == 1) std::snprintf(st.pickerRows[i], 44, "  INIT (bare patch)");
                else { char nm[24]; p5::Engine::patchNameAt((uint32_t)(g - 1), nm);
                       std::snprintf(st.pickerRows[i], 44, "%3d %s", g - 2, nm); }
            }
            else std::snprintf(st.pickerRows[i], 44, "%s", c.userRows[g].c_str());
            p5store::Source row;
            if (c.pickerTab == 0) { row.kind = p5store::SourceKind::Factory;
                                    row.factory = g == 0 ? -2 : g == 1 ? -1 : g - 2; }
            else { row.kind = p5store::SourceKind::File; row.file = c.userRows[g]; }
            auto same = [](const p5store::Source &a, const p5store::Source &b) {
                return a.kind == b.kind && a.factory == b.factory && a.file == b.file; };
            if (same(c.plugin->curSrc_, row)) st.pickerSel = i;
            if (c.bindPending && same(c.bindSrc, row)) st.pickerBind = i;
            st.pickerCount++;
        }
    }
    else if (c.modal == 1 || c.modal == 2 || c.modal == 3 ||
             c.modal == 8 || c.modal == 9)          // 5/6 = pages, not text modals
    {
        st.modalTitle = c.modal == 2 ? "ASSIGN BANK/PROGRAM"
                          : c.modal == 3 || c.modal == 9 ? "OVERWRITE ?"
                          : c.modal == 8 ? "SAVE TUNING" : "SAVE PATCH";
        if (c.modal == 3)
        {
            static char prompt[128];
            std::snprintf(prompt, sizeof prompt,
                          "\"%s\" already exists (name differs only by case)  -  ENTER overwrites",
                          c.modalHit);
            st.modalPrompt = prompt;
        }
        else if (c.modal == 9)
        {
            static char prompt[128];
            std::snprintf(prompt, sizeof prompt,
                          "\"%s\" exists in Tunings  -  ENTER overwrites", c.modalHit);
            st.modalPrompt = prompt;
        }
        else if (c.modal == 8)
        {
            static char prompt[96];
            std::snprintf(prompt, sizeof prompt, "write to  %s/<name>.scl",
                          p5store::tuningsDir().c_str());
            st.modalPrompt = prompt;
        }
        else if (c.modal == 1)
        {
            static char prompt[96];
            std::snprintf(prompt, sizeof prompt, "write to  %s/<name>.muhammad",
                          p5store::baseDir().c_str());
            st.modalPrompt = prompt;
        }
        else
        {
            static char prompt[96];
            std::snprintf(prompt, sizeof prompt,
                          "slot %d-%d  <-  patch name   (or FAC:n for factory 0..%d)",
                          c.modalSlot / 8 + 1, c.modalSlot % 8 + 1, (int)PW_FACTORY_N - 1);
            st.modalPrompt = prompt;
        }
        st.modalText = c.modalBuf;
        st.modalLen = c.modalLen;
    }
    // The digits mean "the quickload slot THIS patch is bound to" (reverse
    // lookup in the retrieval map) — not an index into the factory list. So a
    // remapped slot 2 shows 1-2, and a user patch shows its bound slot too.
    st.bank = program >= 0 ? program / 8 + 1 : 0;
    st.program = program >= 0 ? program % 8 + 1 : 0;
    st.bpSlot[0] = st.bpPend[0] = 0;
    int bound = -1;
    bool bound_ok = p5store::findSlotFor(c.plugin->curSrc_, bound);
    if (!bound_ok && c.plugin->curSrc_.kind == p5store::SourceKind::Factory &&
        c.plugin->curSrc_.factory >= 0 && c.plugin->curSrc_.factory < (int)PW_FACTORY_N &&
        !p5store::hasSlot(c.plugin->curSrc_.factory))
    { bound = c.plugin->curSrc_.factory; bound_ok = true; }   // still on its implicit default slot
    if (bound_ok)
    {
        std::snprintf(st.bpSlot, sizeof st.bpSlot, "%d-%d", bound / 8 + 1, bound % 8 + 1);
        if (c.plugin->bank != bound / 8)                     // cursor wandered: pending in
            std::snprintf(st.bpPend, sizeof st.bpPend, "(%d)", c.plugin->bank + 1);   // fixed LEFT column
    }
    else if (c.plugin->bank != 0)                            // unbound patch, cursor offset
        std::snprintf(st.bpPend, sizeof st.bpPend, "(%d)", c.plugin->bank + 1);
    if (c.bindPending) st.bindHint = "ASSIGN ARMED - PRESS PROGRAM 1..8";
}

// ---- drawing ----------------------------------------------------------------

void renderFrame(GuiCore &c, NVGcontext *vg, p5ui::Gui &gui)
{
    if (!vg || !c.mapped) return;
    p5ui::State st;
    fillState(c, st);
    nvgBeginFrame(vg, (float)c.w, (float)c.h, 1.0f);
    p5ui::draw(gui, st, (float)c.w, (float)c.h);
    nvgEndFrame(vg);
}

// ---- button semantics --------------------------------------------------------

void actuate(GuiCore &c, int btnIdx, unsigned mods)
{
    const auto &b = p5ui::kBtns[btnIdx];
    if (b.pid == -2)                                  // RECORD: save-patch prompt
    {
        c.modal = c.modal == 1 ? 0 : 1;
        c.modalLen = 0; c.modalBuf[0] = 0;
        if (c.modal)
        {
            std::string seed = c.plugin->currentFile();
            if (seed.empty()) seed.assign((const char *)c.plugin->engine.patch().raw + p5::P5_NAME, p5::P5_NAME_LEN);
            while (!seed.empty() && seed.back() == ' ') seed.pop_back();
            std::strncpy(c.modalBuf, seed.c_str(), sizeof c.modalBuf - 1);
            c.modalBuf[sizeof c.modalBuf - 1] = 0;
            c.modalLen = (int)std::strlen(c.modalBuf);
        }
        c.needFocus = true;                           // shim: XSetInputFocus / SetFocus
        return;
    }
    if (b.pid == -3)                                  // PRESET: preset picker (no typing)
    {
        c.modal = c.modal == 4 ? 0 : 4;
        c.pickerTab = 0; c.pickerPage = 0;
        c.modalSlot = c.plugin->assignTarget();
        c.userRows = p5store::listPatches();
        return;
    }
    if (b.pid == -4)                                  // PRESET: recall / toggle-back / basic
    {
        if (mods & P5_MOD_SHIFT) c.plugin->recallBasicInit();  // SHIFT+PRESET = bare INIT
        else c.plugin->presetRecall();                      // press again = re-float
        return;
    }
    if (b.pid == -1)                                  // EDIT: voice V/OCT trim page
    {   std::fprintf(stderr, "[p5gui] EDIT actuate: modal %d -> %d\n", c.modal, c.modal == 5 ? 0 : 5);
        c.modal = (c.modal == 5) ? 0 : 5; c.trimDrag = 0; return; }
    if (b.pid == -6)                                  // TUNING: microtuning editor
    {   std::fprintf(stderr, "[p5gui] TUNING actuate: modal %d -> %d\n", c.modal, c.modal == 6 ? 0 : 6);
        if (c.modal == 6) c.modal = 0;
        else { c.modal = 6; c.tuneFiles = p5store::listTunings(); c.tuneFocus = 0;
               wheelSeed(c);
               c.needFocus = true; }                   // arrow keys land here
        c.tuneDrag = 0; return; }
    if (b.pid < 0) return;                            // decorative
    if (b.ledmode == 2)                               // quickload buttons 1-8
    {
        const int k = c.plugin->bank * 8 + b.value;
        c.plugin->noteProgramClick(b.value);
        if (c.bindPending)
        {   // armed ASSIGN: bind this slot to the picked preset (and load it)
            p5store::setSlot(k, c.bindSrc);
            p5store::saveMap();
            c.plugin->rebuildSlotCache();  // keep the audio-thread cache current
            c.bindPending = false;
        }
        c.plugin->programSlot(k);
        return;
    }
    if (b.pid == PID_BANK)
    {
        const int banks = (int)PW_FACTORY_N / 8;
        const bool down = (mods & (P5_MOD_SHIFT | P5_MOD_RIGHT)) != 0;   // shift-click / right-click
        const int step = down ? banks - 1 : 1;
        c.plugin->guiSet(PID_BANK, (double)((c.plugin->bank + step) % banks));
        return;
    }
    if (b.ledmode == 6)                               // KEYBD>FLT: OFF -> HALF -> FULL cycle
    {
        const int v = (int)(c.plugin->guiValue((clap_id)b.pid) + 0.5);
        c.plugin->guiSet((clap_id)b.pid, (double)((v + 1) % 3));
        return;
    }
    c.plugin->guiSet((clap_id)b.pid, c.plugin->guiValue((clap_id)b.pid) != 0.0 ? 0.0 : 1.0);
}

// ---- pointer / wheel / keyboard state machines --------------------------------

void buildDebugText(GuiCore &c);

bool onPointerPress(GuiCore &c, float px, float py, unsigned mods, unsigned timeMs)
{
    bool dirty = false;
    float lx, ly;
    p5ui::pointToLayout(px, py, (float)c.w, (float)c.h, lx, ly);

    if (c.modal == 10)                       // debug page: COPY button or dismiss
    {
        if (p5ui::dbgpg::hitCopy(lx, ly))
        {   if (!c.dbg[0]) return true;
            const bool ok = c.copyText && c.copyText(c.dbg);
            std::fprintf(stderr, "[p5gui] debug COPY: %s (%u chars)\n",
                         ok ? "clipboard owned" : "FAILED", (unsigned)std::strlen(c.dbg));
            return true;
        }
        c.modal = 0;
        return true;
    }

    if (!c.modal && !(mods & P5_MOD_RIGHT) && timeMs != 0u)
    {   // clicking the status/build strip opens the debug page (Surge-style)
        float sc, xo, yo;
        p5ui::fitWindow((float)c.w, (float)c.h, sc, xo, yo);
        const float winBot = ((float)c.h - yo) / sc, winRight = ((float)c.w - xo) / sc;
        const bool overPanel = p5ui::kPanelH * sc + yo >= (float)c.h - 1.f;
        const bool strip = (lx >= winRight - 640.f) &&
            ((overPanel && ly >= p5ui::kPanelH - 20.f && ly <= p5ui::kPanelH + 2.f) ||
             (!overPanel && ly >= winBot - 24.f && ly <= winBot));
        if (strip)
        {   buildDebugText(c);
            c.modal = 10;
            std::fprintf(stderr, "[p5gui] debug page opened (win %ux%u, chassis %d)\n",
                         c.w, c.h, (int)!overPanel);
            return true;
        }
    }

    if (mods & P5_MOD_RIGHT)
    {   // right-click: trim-rail centring on the EDIT page, BANK step-down elsewhere
        if (c.modal == 5)
        {   int row, col;
            if (p5ui::trim::hitBar(lx, ly, &row, &col) && col >= 0)
            { applyTrimSet(c, row, col, 64); dirty = true; }
            return dirty;
        }
        if (!c.modal)
        {   const int bIdx = p5ui::btnAt(p5ui::State{}, px, py, (float)c.w, (float)c.h);
            if (bIdx >= 0 && p5ui::kBtns[bIdx].pid == PID_BANK)
            { actuate(c, bIdx, P5_MOD_RIGHT); dirty = true; }
        }
        return dirty;
    }

    if (c.modal == 4)
    {
        if (!p5ui::pick::inside(lx, ly)) { c.modal = 0; return true; }
        const int tab = p5ui::pick::hitTab(lx, ly);
        if (tab >= 0) { c.pickerTab = tab; c.pickerPage = 0; return true; }
        const int pg = p5ui::pick::hitPage(lx, ly);
        if (pg != 0) { c.pickerPage += (pg < 0 ? -1 : 1);
            if (c.pickerPage < 0) c.pickerPage = 0; return true; }
        if (p5ui::pick::hitClose(lx, ly)) { c.modal = 0; return true; }
        auto srcAt = [&](int row, p5store::Source &s) -> bool {
            const int g = c.pickerPage * p5ui::pick::rows + row;
            if (c.pickerTab == 0)
            {
                if (g >= (int)PW_FACTORY_N + 2) return false;
                s.kind = p5store::SourceKind::Factory;
                s.factory = g == 0 ? -2 : g == 1 ? -1 : g - 2;
            }
            else
            {
                if (g >= (int)c.userRows.size()) return false;
                s.kind = p5store::SourceKind::File; s.file = c.userRows[g];
            }
            return true;
        };
        p5store::Source s;
        const int chip = p5ui::pick::hitAssign(lx, ly);
        if (chip >= 0 && srcAt(chip, s))
        {   // ASSIGN: arm the bind, close the picker so 1-8 are reachable
            c.bindPending = !(c.bindPending &&
                c.bindSrc.kind == s.kind && c.bindSrc.factory == s.factory &&
                c.bindSrc.file == s.file);
            c.bindSrc = s;
            c.modal = 0; return true;
        }
        const int row = p5ui::pick::hitRow(lx, ly);
        if (row >= 0 && srcAt(row, s))
        {   // row body: LOAD ONLY — quickload slots untouched, picker stays open
            c.plugin->loadSource(s);
            dirty = true;
        }
        return dirty;
    }
    if (c.modal == 5)
    {
        if (!p5ui::trim::inside(lx, ly)) { c.modal = 0; return true; }
        int row, col;
        if (p5ui::trim::hitBar(lx, ly, &row, &col) && col >= 0)
        {   c.trimDrag = 1; c.trimRow = row; c.trimCol = col;
            applyTrimDrag(c, lx);
            dirty = true;                             // shim grabs the pointer (trimDrag armed)
        }
        return dirty;                                 // page is modal: swallow the press
    }
    if (c.modal == 7)                                 // tuning file picker
    {
        if (!p5ui::pick::inside(lx, ly)) { c.modal = 6; return true; }
        const int tab = p5ui::pick::hitTab(lx, ly);
        if (tab >= 0) { c.tuneTab = tab; c.tunePage = 0; return true; }
        const int totalRows = 1 + (c.tuneTab ? (int)c.tuneFiles.size() : P5_STOCK_N);
        const int pages = (totalRows + p5ui::pick::rows - 1) / p5ui::pick::rows;
        const int pg = p5ui::pick::hitPage(lx, ly);
        if (pg)
        {   c.tunePage += pg;
            if (c.tunePage < 0) c.tunePage = 0;
            if (c.tunePage >= pages) c.tunePage = pages - 1;
            return true; }
        const int r = p5ui::pick::hitRow(lx, ly);
        std::fprintf(stderr, "[p5gui] tunepicker press (%.0f,%.0f) tab=%d r=%d\n", lx, ly, c.tuneTab, r);
        if (r >= 0)
        {   const int idx = c.tunePage * p5ui::pick::rows + r;
            if (idx == 0)
            {   c.plugin->engine.setTuningDeviation(nullptr);
                std::snprintf(c.plugin->tuneName_, sizeof c.plugin->tuneName_, "12-TET");
                c.plugin->tuneToneCount_ = 0; c.wheelN = 0; c.wheelHot = -1;
                c.wheelMask = 0; c.wheelWalk = 0;
                std::fprintf(stderr, "[p5gui] tuning: standard 12-TET selected\n"); }
            else if (c.tuneTab == 0 && idx - 1 < P5_STOCK_N)
            {   applyStock(c, idx - 1); c.modal = 6; return true; }
            else if (c.tuneTab == 1 && idx - 1 < (int)c.tuneFiles.size())
            {   static int16_t dev[128]; char nm[40];
                static double tn2[64]; int tnn = 0;
                bool kbm = false;
                if (p5store::loadTuningDev(c.tuneFiles[idx - 1], dev, nm, sizeof nm, tn2, &tnn, &kbm))
                {   std::snprintf(c.plugin->tuneName_, sizeof c.plugin->tuneName_, "%s", nm);
                    c.plugin->tuneToneCount_ = tnn;
                    for (int k = 0; k < tnn && k < 64; k++) c.plugin->tuneTones_[k] = tn2[k];
                    if (!kbm)
                    {   // .scl carries degrees only - re-apply melodee semantics ourselves:
                        // <=12 tones quantize to the nearest degree, >12 walk (ALL mode).
                        double p[66]; p[0] = 0.0;
                        for (int k = 0; k < tnn && k < 64; k++) p[k + 1] = tn2[k];
                        const uint16_t m = p5store::deriveMask(tn2, tnn);
                        static int16_t devQ[128];
                        if (p5store::buildScaleDev(p, tnn, m, c.wheelRoot, devQ))
                        {   std::memcpy(dev, devQ, sizeof dev);
                            c.plugin->tuneMask_ = m;
                            c.plugin->tuneWalk_ = (m == 0);
                        }
                    }
                    c.plugin->engine.setTuningDeviation(dev);
                    wheelSeed(c);
                    std::fprintf(stderr, "[p5gui] tuning loaded: %s (%d tones editable)\n", nm, tnn); }
                else std::fprintf(stderr, "[p5gui] tuning LOAD failed: %s\n",
                                  c.tuneFiles[idx - 1].c_str());
            }
            c.modal = 6; return true;
        }
        return dirty;
    }
    if (c.modal == 6)                                 // microtuning editor page
    {
        if (!p5ui::tune::inside(lx, ly)) { c.modal = 0; return true; }
        {   if (!c.wheelN)                        // first touch of the grid = adopt 12-TET
            {   c.wheelN = 12;
                for (int k = 1; k <= 12; k++) c.wheelC[k - 1] = 100.f * k;
            }
            const int wn = p5ui::tune::nodeAt(c.wheelC, c.wheelN, lx, ly);
            if (wn >= 0)
            {   c.wheelHot = wn;
                const bool dbl = (wn == c.wheelLastIdx &&
                                  (unsigned)(timeMs - c.wheelLastMs) < 450u);
                c.wheelLastMs = timeMs; c.wheelLastIdx = wn;
                if (dbl)                          // double-click: snap this tone to equal
                {   const float et = p5ui::tune::etCents(c.wheelC, c.wheelN);
                    const float nc = p5ui::tune::clampTone(c.wheelC, c.wheelN, wn, et * wn);
                    if (nc != c.wheelC[wn - 1])
                    {   c.wheelC[wn - 1] = nc; wheelCommit(c);
                        std::fprintf(stderr, "[p5gui] wheel tone %d snapped to equal\n", wn); }
                    return true;
                }
                c.wheelDragIdx = wn;
                c.wheelDragC0 = c.wheelC[wn - 1];
                c.wheelDragDist0 = std::hypot(lx - p5ui::tune::wheelCX, ly - p5ui::tune::wheelCY);
                return true;                          // shim grabs the pointer (wheelDragIdx armed)
            }
        }
        const int ctl = p5ui::tune::hitControl(lx, ly);
        if (ctl == 1) { c.modal = 7; c.tunePage = 0; c.tuneTab = 0; return true; }
        else if (ctl == 5)
        {   c.modal = 8; c.modalLen = 0; c.modalBuf[0] = 0;
            std::string seed(c.plugin->tuneName_);
            if (!seed.empty() && seed.back() == '*') seed.pop_back();
            while (!seed.empty() && seed.back() == ' ') seed.pop_back();
            if (seed.empty()) seed = "my tuning";
            std::strncpy(c.modalBuf, seed.c_str(), sizeof c.modalBuf - 1);
            c.modalBuf[sizeof c.modalBuf - 1] = 0;
            c.modalLen = (int)std::strlen(c.modalBuf);
            c.needFocus = true;
            return true; }
        else if (ctl == 6 || ctl == 7)
        {   c.wheelRoot = (c.wheelRoot + (ctl == 7 ? 1 : -1) + 12) % 12;
            wheelApply(c);
            std::fprintf(stderr, "[p5gui] tuning root -> %d\n", c.wheelRoot);
            return true; }
        else if (ctl == 2)
        {   c.plugin->engine.setTuningDeviation(nullptr);
            std::snprintf(c.plugin->tuneName_, sizeof c.plugin->tuneName_, "12-TET");
            c.plugin->tuneToneCount_ = 0; c.wheelHot = -1;
            c.wheelN = 12; c.wheelMask = 0x0fff; c.wheelWalk = 0;
            for (int i = 0; i < 12; i++) c.wheelC[i] = 100.f * (i + 1);  // editable grid
            std::fprintf(stderr, "[p5gui] tuning RESET to 12-TET\n");
            return true; }
        else if (ctl >= 3)
        {   c.tuneDrag = ctl; c.tuneFocus = ctl - 3; applyTuneDrag(c, lx, mods);
            return true;                              // shim grabs the pointer (tuneDrag armed)
        }
        return dirty;                                 // page is modal: swallow the press
    }
    c.dragKnob = p5ui::knobAt(p5ui::State{}, px, py, (float)c.w, (float)c.h);
    if (c.dragKnob >= 0)
    {
        c.dragStartY = (int)py;
        c.dragStartNorm = p5::knobNorm(p5ui::kKnobs[c.dragKnob].pid,
                                       c.plugin->guiValue(p5ui::kKnobs[c.dragKnob].pid));
        c.pressBtn = -1;
        // no dirty: the press itself changes nothing visually (as before);
        // motion events paint the drag. The shim grabs (dragKnob armed).
    }
    else
    {
        c.pressBtn = p5ui::btnAt(p5ui::State{}, px, py, (float)c.w, (float)c.h);
    }
    return dirty;
}

bool onPointerRelease(GuiCore &c, float px, float py, unsigned mods)
{
    bool dirty = false;
    if (c.pressBtn >= 0 &&
        p5ui::btnAt(p5ui::State{}, px, py, (float)c.w, (float)c.h) == c.pressBtn)
    { actuate(c, c.pressBtn, mods); dirty = true; }  // LEDs flip NOW
    if (c.dragKnob >= 0)                        // gesture ends: tell the host the FINAL value
    { c.plugin->guiSync((clap_id)p5ui::kKnobs[c.dragKnob].pid); dirty = true; }
    c.dragKnob = -1; c.pressBtn = -1; c.trimDrag = 0; c.tuneDrag = 0;
    c.wheelDragIdx = 0;
    return dirty;
}

bool onPointerMotion(GuiCore &c, float px, float py, unsigned mods)
{
    bool dirty = false;
    float lx, ly;
    if (c.wheelDragIdx)
    {
        p5ui::pointToLayout(px, py, (float)c.w, (float)c.h, lx, ly);
        applyWheelDrag(c, lx, ly, mods);
        return true;
    }
    if (c.tuneDrag)
    {
        p5ui::pointToLayout(px, py, (float)c.w, (float)c.h, lx, ly);
        applyTuneDrag(c, lx, mods);
        return true;
    }
    if (c.trimDrag)
    {
        p5ui::pointToLayout(px, py, (float)c.w, (float)c.h, lx, ly);
        applyTrimDrag(c, lx);
        dirty = true;
    }
    if (c.dragKnob >= 0)
    {
        const double dy = (double)(c.dragStartY - (int)py);
        double norm = c.dragStartNorm + dy * p5ui::kDragRangePer100px / 100.0;
        if (mods & P5_MOD_SHIFT)   // fine: rebase from start
            norm = c.dragStartNorm + dy * p5ui::kDragRangePer100px / 100.0 * 0.2;
        if (norm < 0) norm = 0;
        if (norm > 1) norm = 1;
        const int pid = p5ui::kKnobs[c.dragKnob].pid;
        c.plugin->guiSetLocal((clap_id)pid, normToValue(pid, norm));
        dirty = true;   // rendered once after the burst drains
    }
    return dirty;
}

bool onWheel(GuiCore &c, float px, float py, int dir)   // dir: -1 up, +1 down/next page
{
    if (c.modal == 7)                     // tuning picker pages on the wheel
    {
        const int total = 1 + (c.tuneTab ? (int)c.tuneFiles.size() : P5_STOCK_N);
        const int pages = (total + p5ui::pick::rows - 1) / p5ui::pick::rows;
        c.tunePage += dir;
        if (c.tunePage < 0) c.tunePage = 0;
        if (c.tunePage >= pages) c.tunePage = pages - 1;
        return true;
    }
    if (c.modal == 4)                     // patch picker likewise
    {
        const int total = c.pickerTab == 0 ? (int)PW_FACTORY_N + 2 : (int)c.userRows.size();
        const int pages = total ? (total + p5ui::pick::rows - 1) / p5ui::pick::rows : 1;
        c.pickerPage += dir;
        if (c.pickerPage < 0) c.pickerPage = 0;
        if (c.pickerPage >= pages) c.pickerPage = pages - 1;
        return true;
    }
    const int k = p5ui::knobAt(p5ui::State{}, px, py, (float)c.w, (float)c.h);
    if (k >= 0)
    {
        const int pid = p5ui::kKnobs[k].pid;
        double v = c.plugin->guiValue(pid);
        const p5::KnobRange *r = p5::knobRange(pid);
        const double step = r ? (r->hi - r->lo) / 50.0 : 5.0;
        v += (dir < 0 ? step : -step);
        c.plugin->guiSet((clap_id)pid, normToValue(pid, p5::knobNorm(pid, v)));
        return true;
    }
    return false;
}

bool onKey(GuiCore &c, GuiKey key, unsigned mods)
{
    if (c.modal == 10)
    {   if (key == K_ESCAPE) c.modal = 0;
        return true;                     // the page swallows everything else
    }
    if (!c.modal) return false;
    if (key == K_ESCAPE)
    {   if (c.modal == 3) c.modal = 1;
        else if (c.modal == 9) c.modal = 8;
        else if (c.modal == 8) c.modal = 6;
        else c.modal = 0;
        return true; }
    if (key == K_BACKSPACE) { if (c.modalLen > 0) c.modalBuf[--c.modalLen] = 0; return true; }
    if (key == K_RETURN)
    {
        c.modalBuf[c.modalLen] = 0;
        if (c.modal == 1)
        {
            const std::string hit = p5store::foldCollision(c.modalBuf);
            if (!hit.empty())     // faithful case, but "peace"==... -> confirm
            {
                std::snprintf(c.modalHit, sizeof c.modalHit, "%s", hit.c_str());
                c.modal = 3; return true;
            }
            c.plugin->savePatchNamed(c.modalBuf);
            c.modal = 0; c.modalLen = 0;
        }
        else if (c.modal == 2) { c.plugin->assignSlotFromText(c.modalSlot, c.modalBuf); c.modal = 0; c.modalLen = 0; }
        else if (c.modal == 3) { c.plugin->savePatchNamed(c.modalBuf); c.modal = 0; c.modalLen = 0; }
        else if (c.modal == 8)
        {   if (p5store::tuningFileExists(c.modalBuf))
            {   std::snprintf(c.modalHit, sizeof c.modalHit, "%s", c.modalBuf);
                c.modal = 9; return true; }
            static double tt[64];
            for (int i = 0; i < c.wheelN && i < 64; i++) tt[i] = c.wheelC[i];
            if (p5store::saveTuningScl(c.modalBuf, tt, c.wheelN))
            {   std::snprintf(c.plugin->tuneName_, sizeof c.plugin->tuneName_, "%s", c.modalBuf);
                std::fprintf(stderr, "[p5gui] tuning saved: %s.scl\n", c.modalBuf);
                c.tuneFiles = p5store::listTunings(); }
            c.modal = 6; c.modalLen = 0; }
        else if (c.modal == 9)
        {   static double tt[64];
            for (int i = 0; i < c.wheelN && i < 64; i++) tt[i] = c.wheelC[i];
            if (p5store::saveTuningScl(c.modalBuf, tt, c.wheelN))
            {   std::snprintf(c.plugin->tuneName_, sizeof c.plugin->tuneName_, "%s", c.modalBuf);
                std::fprintf(stderr, "[p5gui] tuning saved (overwrite): %s.scl\n", c.modalBuf);
                c.tuneFiles = p5store::listTunings(); }
            c.modal = 6; c.modalLen = 0; }
        return true;
    }
    if (c.modal == 6)                     // tuning page: arrows adjust the focused range rail
    {
        float up = (float)c.plugin->engine.bendRangeUpDefault();
        float dn = (float)c.plugin->engine.bendRangeDownDefault();
        const float d = (mods & P5_MOD_SHIFT) ? 1.0f : 0.1f;  // Shift = integer step
        if (key == K_LEFT) c.tuneFocus = 0;
        else if (key == K_RIGHT) c.tuneFocus = 1;
        else if (key == K_UP)    { if (c.tuneFocus) dn += d; else up += d; }
        else if (key == K_DOWN) { if (c.tuneFocus) dn -= d; else up -= d; }
        else return false;
        auto clampQ = [](float v) { v = v < 0.f ? 0.f : v > 24.f ? 24.f : v;
                                        return (float)((int)(v * 10.0f + 0.5f)) / 10.0f; };
        c.plugin->engine.setBendRangeDefault(clampQ(up), clampQ(dn));
        return true;
    }
    return false;
}

bool onChar(GuiCore &c, unsigned int ch)
{
    if (!c.modal) return false;
    const char qc = (char)ch;
    if ((qc >= 'A' && qc <= 'Z') || (qc >= 'a' && qc <= 'z') ||
        (qc >= '0' && qc <= '9') || qc == ' ' || qc == '.' || qc == '_' || qc == '-')
    {   // faithful case: the platform already applied shift (XLookupString / WM_CHAR)
        if (c.modalLen < (int)sizeof c.modalBuf - 1)
            c.modalBuf[c.modalLen++] = qc;
        return true;
    }
    return false;
}

// ---- aspect negotiation -------------------------------------------------------

bool coreMirrorSlot(GuiCore &c, int slotW, int slotH, uint32_t &outW, uint32_t &outH,
                    long nowMs)
{
    if (slotW <= 1 || slotH <= 1) return false;
    const uint32_t wantH = (uint32_t)((uint64_t)slotW * p5ui::kPanelH / p5ui::kPanelW);

    // The ASK is its own state machine, evaluated on EVERY pass: a host that
    // resizes our child window directly to its oversized slot (Bitwig) makes
    // slot == current size, and an early-out before the ask meant we never
    // asked and never trimmed — the "rr 0/3, giant black window" deadlock.
    if (slotH > (long)wantH + 20 && c.hostGui && c.hostGui->request_resize &&
        c.totalAsks < 3 && nowMs >= c.askNextMs && !c.giveUpOnHost)
    {
        c.askNextMs = nowMs + 1200;
        const bool ok = c.hostGui->request_resize(c.plugin->host, (uint32_t)slotW, wantH);
        c.resizeRequests++;
        c.totalAsks++;
        c.askLastRet = ok ? 1 : 0;
        if (ok) c.acceptedAtMs = nowMs;
        std::fprintf(stderr, "[p5gui] request_resize(%d,%u) -> host %s (ask %d/3)\n",
                     slotW, wantH, ok ? "ACCEPTED" : "REFUSED", c.totalAsks);
        // A host may answer REFUSED (spec-legal: "rejected"). Treat any
        // answered ask that left us oversized as non-cooperative.
        if (!ok) c.giveUpOnHost = true;
    }

    if ((uint32_t)slotW == c.w && (uint32_t)slotH == c.h) return false;
    static int mirrorLogged = 0;
    if (!mirrorLogged++)
        std::fprintf(stderr, "[p5gui] mirroring parent geometry -> %dx%d\n", slotW, slotH);
    // Until the window is actually right-sized (host complied, or the shim's
    // top-level trim landed), the excess is OURS and gets chassis, not
    // host-black. This is transient by design, not the steady state.
    outW = (uint32_t)slotW; outH = (uint32_t)slotH;
    c.w = outW; c.h = outH;
    return true;
}
void buildDebugText(GuiCore &c)
{
    Plugin *P = c.plugin;
    int n = 0;
    auto line = [&](const char *fmt, ...)
    {
        if (n >= (int)sizeof c.dbg - 8) return;
        va_list ap; va_start(ap, fmt);
        n += std::vsnprintf(c.dbg + n, sizeof c.dbg - (size_t)n, fmt, ap);
        va_end(ap);
        if (n > 0 && c.dbg[n - 1] == '\n') {} else if (n < (int)sizeof c.dbg - 8) { c.dbg[n++] = '\n'; }
    };
    const char *name = "";
    {   const std::string cur = P->currentFile();
        name = cur.empty() ? (const char *)P->engine.patch().raw + p5::P5_NAME : cur.c_str(); }
    line("Muhammad build: %s", P5_BUILD_INFO);
    {   const clap_host_t *hh = P->host;
        const char *nm = (hh && hh->name) ? hh->name : "?";
        const char *vd = (hh && hh->vendor) ? hh->vendor : "";
        const char *vr = (hh && hh->version) ? hh->version : "";
        line("host: %.40s%s%.20s v%.16s  wm[%.24s]  pid %ld",
             nm, *vd ? " / " : "", vd, vr, c.hostName, (long)getpid());
    }
    {   static const char *const kProbe[] = { CLAP_EXT_GUI, CLAP_EXT_TIMER_SUPPORT, CLAP_EXT_LOG,
            CLAP_EXT_LATENCY, CLAP_EXT_AUDIO_PORTS_CONFIG, CLAP_EXT_NOTE_NAME, CLAP_EXT_STATE };
        char exts[160] = {}; int en = 0;
        auto *hh = P->host;
        for (unsigned q = 0; q < sizeof kProbe / sizeof *kProbe && en < 150; q++)
        {   const void *x = (hh && hh->get_extension) ? hh->get_extension(hh, kProbe[q]) : nullptr;
            if (x) en += std::snprintf(exts + en, sizeof exts - (size_t)en, "%s%s", en ? " " : "", kProbe[q] + 5); }
        line("host ext: %.150s", exts[0] ? exts : "(none)");
    }
    line("gui: api %s  clap.gui svc %s  timer %s  scale %.2f",
         c.guiApi, c.hostGui ? "present" : "absent",
         c.timerRegistered ? "registered" : "none", c.scale);
    line("negotiation: asks %d/3 %s  top-trim %d/5 give-up %d  chassis %d",
         c.totalAsks,
         c.askLastRet < 0 ? "none" : (c.askLastRet ? "ACCEPTED" : "REFUSED"),
         c.topTries, (int)c.giveUpOnHost,
         (int)((float)c.h > (float)c.w * p5ui::kPanelH / p5ui::kPanelW + 3.f));
    line("window: win %ux%u  parent %ux%u  ancestor %ux%u",
         c.w, c.h, c.parW, c.parH, c.ancW, c.ancH);
    line("audio: sr %.0f  block %u  oversample %d  polyphony %d  live %u  simd %s",
         P->sr, P->minBlock, P->engine.oversample(), P->engine.polyphony(),
         (unsigned)P->engine.liveVoices(), P->engine.simdPath());
    line("patch: src %s  name %.20s", P->loadTag_, name);
    line("tuning: %.60s  tones %d  root %d  mask %03x  walk %d",
         P->tuneName_, (int)P->tuneToneCount_, P->tuneRoot_, (unsigned)P->tuneMask_,
         P->tuneWalk_);
    line("state: modal %d  tuneFocus %d  wheelN %d  pickerTab %d page %d",
         c.modal, c.tuneFocus, c.wheelN, c.pickerTab, c.pickerPage);
    line("store: %s", p5store::baseDir().c_str());
    if (n < (int)sizeof c.dbg - 1) c.dbg[n] = 0;
}

bool coreShouldTrimNow(GuiCore &c, long nowMs)
{
    // host answered REFUSED, ran out of asks, or ACCEPTED and then kept the
    // oversized slot for > 2 s (Bitwig-style lie). Re-armed only when a real
    // configure lands us back at the aspect (giveUpOnHost cleared by shim).
    if (c.giveUpOnHost) return true;
    if (c.totalAsks >= 3) return true;
    if (c.askLastRet == 1 && c.acceptedAtMs != 0 && nowMs - c.acceptedAtMs > 2000) return true;
    return false;
}

void coreAncestorAsk(GuiCore &c, uint32_t parW, uint32_t parH,
                     uint32_t ancW, uint32_t ancH, long nowMs)
{
    c.parW = parW; c.parH = parH; c.ancW = ancW; c.ancH = ancH;
    // Bitwig sizes our immediate container to the aspect we report; wasted
    // space may live one level up. While still polite ask the host for the
    // ancestor's height (the shim gates this to ~1/s using c.lastAskMs).
    if (c.hostGui && c.hostGui->request_resize && c.resizeRequests < 2 &&
        c.ancH > c.parH + 60 && c.parH > 1)
    {
        c.lastAskMs = nowMs;
        c.resizeRequests++;
        const bool ok2 = c.hostGui->request_resize(c.plugin->host, c.parW, c.ancH);
        std::fprintf(stderr, "[p5gui] ancestor ask(%u,%u) -> host %s\n",
                     c.parW, c.ancH, ok2 ? "ACCEPTED" : "REFUSED");
    }
}

// ---- host timer registration (pure CLAP) --------------------------------------

void registerTimer(GuiCore &c)
{
    auto *host = c.plugin->host;
    if (!host || !host->get_extension) return;
    auto *ts = (const clap_host_timer_support_t *)host->get_extension(host, CLAP_EXT_TIMER_SUPPORT);
    if (ts && ts->register_timer && ts->register_timer(host, 16, &c.timerId))
        c.timerRegistered = true;
    std::fprintf(stderr, "[p5gui] timer-support %s\n", c.timerRegistered ? "registered" : "absent");
}

void unregisterTimer(GuiCore &c)
{
    auto *host = c.plugin->host;
    if (!c.timerRegistered || !host || !host->get_extension) return;
    auto *ts = (const clap_host_timer_support_t *)host->get_extension(host, CLAP_EXT_TIMER_SUPPORT);
    if (ts && ts->unregister_timer) ts->unregister_timer(host, c.timerId);
    c.timerRegistered = false;
}

} // namespace pw
