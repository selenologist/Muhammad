// SPDX-License-Identifier: GPL-3.0-only
// clap-gui backend: an X11 child window embedded in the host's window, GLX +
// NanoVG GL3 rendering the shared p5ui panel, mouse gestures back into CLAP
// params (via Plugin::guiSet, drained to the host on the next params.flush).
// Linux is the primary target; win32/cocoa tables can follow the same shape.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <GL/glx.h>
#include <GL/gl.h>

#include <nanovg.h>
#define NANOVG_GL3 1
#include <nanovg_gl.h>

#include <clap/clap.h>
#include <clap/ext/timer-support.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "muhammad_plugin.hpp"
#include "p5_common.hpp"
#include "p5_gui.hpp"
#include "p5_store.hpp"
#include "p5_stock_scales.hpp"

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
    int dragStartY = 0;
    double dragStartNorm = 0.0;
    bool pointerGrabbed = false;

    clap_id timerId = 0;
    bool timerRegistered = false;

    uint32_t reqW = 0, reqH = 0;   // set_size may arrive before set_parent
    uint32_t parW = 0, parH = 0, ancW = 0, ancH = 0;   // wrapper / ancestor sizes (diag)

    // host-side gui service + aspect negotiation bookkeeping
    const clap_host_gui_t *hostGui = nullptr;
    int resizeRequests = 0;        // give up if the host keeps saying no
    int lastRrOk = -1;               // last request_resize verdict
    long lastAskMs = 0;
};

Ctx *ctxFrom(const clap_plugin_t *p)
{
    auto *s = static_cast<Plugin *>(p->plugin_data);
    return s ? static_cast<Ctx *>(s->gui) : nullptr;
}

// ---- state <-> layout -------------------------------------------------------

void applyTrimSet(Ctx &c, int row, int col, int v)
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

void applyTrimDrag(Ctx &c, float lx)
{   applyTrimSet(c, c.trimRow, c.trimCol, p5ui::trim::valueAt(c.trimCol, lx));
}

void wheelApply(Ctx &c);

void wheelSeed(Ctx &c)
{   const Plugin *P = c.plugin;
    c.wheelN = (P->tuneToneCount_ >= 2 && P->tuneToneCount_ <= 64) ? P->tuneToneCount_ : 0;
    for (int i = 0; i < c.wheelN; i++) c.wheelC[i] = (float)P->tuneTones_[i];
    c.wheelMask = P->tuneMask_; c.wheelWalk = P->tuneWalk_; c.wheelRoot = P->tuneRoot_;
}

void applyStock(Ctx &c, int si)
{   const StockScale &sc = P5_STOCK[si];
    c.wheelN = sc.degrees;
    for (int i = 0; i < sc.degrees; i++) c.wheelC[i] = sc.p[i + 1];   // drop unison, keep period
    c.wheelMask = sc.mask; c.wheelWalk = sc.walk;
    wheelApply(c);
    std::snprintf(c.plugin->tuneName_, sizeof c.plugin->tuneName_, "%s", sc.title);
    std::fprintf(stderr, "[p5gui] stock scale: %s (%d tones, mask %03x, %s)\n",
                 sc.title, sc.degrees, sc.mask, sc.walk ? "ALL-walk" : "quantized");
}

void wheelApply(Ctx &c)
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

void wheelCommit(Ctx &c)         // user edited: apply + mark the name
{   wheelApply(c);
    char *n = c.plugin->tuneName_;
    if (!std::strchr(n, '*') && std::strlen(n) + 3 < sizeof c.plugin->tuneName_)
        std::strncat(n, " *", sizeof c.plugin->tuneName_ - std::strlen(n) - 1);
}

void applyWheelDrag(Ctx &c, float lx, float ly, unsigned mods)
{   const int i = c.wheelDragIdx;
    if (i < 1 || i > c.wheelN) return;
    const float dist = std::hypot(lx - p5ui::tune::wheelCX, ly - p5ui::tune::wheelCY);
    const float et = p5ui::tune::etCents(c.wheelC, c.wheelN);
    float dc = (dist - c.wheelDragDist0) / p5ui::tune::ringScale(c.wheelC, c.wheelN) * et;
    if (mods & ShiftMask) dc *= 0.1f;                       // fine edit (Surge's shift speed)
    const float nc = p5ui::tune::clampTone(c.wheelC, c.wheelN, i, c.wheelDragC0 + dc);
    if (nc != c.wheelC[i - 1]) { c.wheelC[i - 1] = nc; wheelCommit(c); }
}

void applyTuneDrag(Ctx &c, float lx, unsigned mods)
{   const int k = c.tuneDrag - 3;
    float v = p5ui::tune::railValAt(k, lx);
    if (mods & ShiftMask) v = (float)(int)(v + 0.5f);   // Shift: snap to integer semitones
    const float up = k == 0 ? v : (float)c.plugin->engine.bendRangeUpDefault();
    const float dn = k == 1 ? v : (float)c.plugin->engine.bendRangeDownDefault();
    c.plugin->engine.setBendRangeDefault(up, dn);   // resolved pair stored (WYSIWYG)
}

void fillState(Ctx &c, p5ui::State &st)
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

void actuate(Ctx &c, int btnIdx, unsigned mods = 0)
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
        XSetInputFocus(c.dpy, c.win, RevertToParent, CurrentTime);
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
        if (mods & ShiftMask) c.plugin->recallBasicInit();  // SHIFT+PRESET = bare INIT
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
               XSetInputFocus(c.dpy, c.win, RevertToParent, CurrentTime); }   // arrow keys land here
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
        const bool down = (mods & (ShiftMask | (1u << 30))) != 0;   // shift-click / right-click
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

long nowMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// askHostForAspect REMOVED (v1.7): chassis mode owns surplus height;
// shrinking it via request_resize would directly fight the grow request.


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
            // Never copy a too-tall parent: our own window is exactly panel-aspect
            // of the slot's width (hosts hand out square slots until asked).
            const uint32_t wantH = (uint32_t)((uint64_t)pa.width * p5ui::kPanelH / p5ui::kPanelW);
            XResizeWindow(c.dpy, c.win, (uint32_t)pa.width, wantH);
            c.w = (uint32_t)pa.width; c.h = wantH;
            dirty = true;
            if (pa.height > wantH + 20 && c.hostGui && c.hostGui->request_resize &&
                c.resizeRequests < 3)
            {   c.resizeRequests++;
                std::fprintf(stderr, "[p5gui] slot %dx%d too tall; request_resize(%d,%u)\n",
                             pa.width, pa.height, pa.width, wantH);
                c.hostGui->request_resize(c.plugin->host, pa.width, wantH);
            }
        }
    }
    // Bitwig sizes our immediate container to the aspect we report; wasted
    // space may live one level up. Measure both every ~1s (diag shows them:
    // a screenshot then proves WHICH window owns any dead band), and while
    // still polite ask the host for the ancestor's height.
    {
        const long t = nowMs();
        if (t - c.lastAskMs >= 1000)
        {
            XWindowAttributes pa{};
            if (c.parent && XGetWindowAttributes(c.dpy, c.parent, &pa))
            {
                c.parW = pa.width; c.parH = pa.height;
                Window r, pp = 0, *kids = nullptr; unsigned nk = 0;
                c.ancW = c.ancH = 0;
                if (XQueryTree(c.dpy, c.parent, &r, &pp, &kids, &nk) && pp)
                {
                    XWindowAttributes ga{};
                    if (XGetWindowAttributes(c.dpy, pp, &ga)) { c.ancW = ga.width; c.ancH = ga.height; }
                }
                if (kids) XFree(kids);
                if (c.hostGui && c.hostGui->request_resize && c.resizeRequests < 2 &&
                    c.ancH > c.parH + 60 && c.parH > 1)
                {
                    c.lastAskMs = t;
                    c.resizeRequests++;
                    c.hostGui->request_resize(c.plugin->host, c.parW, c.ancH);
                }
            }
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
                if (ev.xexpose.count == 0) dirty = true;
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0 &&
                    ((uint32_t)ev.xconfigure.width != c.w || (uint32_t)ev.xconfigure.height != c.h))
                {
                    c.w = (uint32_t)ev.xconfigure.width;
                    c.h = (uint32_t)ev.xconfigure.height;
                    c.resizeRequests = 0;   // host answered (or user dragged): renegotiate
                    dirty = true;
                }
                break;
            case ButtonPress:
                if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
                {
                    const int dir = ev.xbutton.button == Button4 ? -1 : +1;   // wheel down = next page
                    if (c.modal == 7)                     // tuning picker pages on the wheel
                    {
                        const int total = 1 + (c.tuneTab ? (int)c.tuneFiles.size() : P5_STOCK_N);
                        const int pages = (total + p5ui::pick::rows - 1) / p5ui::pick::rows;
                        c.tunePage += dir;
                        if (c.tunePage < 0) c.tunePage = 0;
                        if (c.tunePage >= pages) c.tunePage = pages - 1;
                        dirty = true;
                        break;
                    }
                    if (c.modal == 4)                     // patch picker likewise
                    {
                        const int total = c.pickerTab == 0 ? (int)PW_FACTORY_N + 2 : (int)c.userRows.size();
                        const int pages = total ? (total + p5ui::pick::rows - 1) / p5ui::pick::rows : 1;
                        c.pickerPage += dir;
                        if (c.pickerPage < 0) c.pickerPage = 0;
                        if (c.pickerPage >= pages) c.pickerPage = pages - 1;
                        dirty = true;
                        break;
                    }
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
                if (ev.xbutton.button == Button3 && c.modal == 5)
                {   // right-click on a trimmer rail = centre it (in tune)
                    float lx, ly;
                    p5ui::pointToLayout((float)ev.xbutton.x, (float)ev.xbutton.y,
                                        (float)c.w, (float)c.h, lx, ly);
                    int row, col;
                    if (p5ui::trim::hitBar(lx, ly, &row, &col) && col >= 0)
                    { applyTrimSet(c, row, col, 64); dirty = true; }
                    break;
                }
                if (ev.xbutton.button == Button3)
                {   // right-click on BANK = step down
                    if (!c.modal)
                    {
                        const int bIdx = p5ui::btnAt(p5ui::State{}, (float)ev.xbutton.x, (float)ev.xbutton.y,
                                                     (float)c.w, (float)c.h);
                        if (bIdx >= 0 && p5ui::kBtns[bIdx].pid == PID_BANK)
                        { actuate(c, bIdx, 1u << 30); dirty = true; }
                    }
                    break;
                }
                if (ev.xbutton.button != Button1) break;
                if (c.modal == 4)
                {
                    float lx, ly;
                    p5ui::pointToLayout((float)ev.xbutton.x, (float)ev.xbutton.y,
                                        (float)c.w, (float)c.h, lx, ly);
                    if (!p5ui::pick::inside(lx, ly)) { c.modal = 0; dirty = true; break; }
                    const int tab = p5ui::pick::hitTab(lx, ly);
                    if (tab >= 0) { c.pickerTab = tab; c.pickerPage = 0; dirty = true; break; }
                    const int pg = p5ui::pick::hitPage(lx, ly);
                    if (pg != 0) { c.pickerPage += (pg < 0 ? -1 : 1);
                        if (c.pickerPage < 0) c.pickerPage = 0; dirty = true; break; }
                    if (p5ui::pick::hitClose(lx, ly)) { c.modal = 0; dirty = true; break; }
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
                        c.modal = 0; dirty = true; break;
                    }
                    const int row = p5ui::pick::hitRow(lx, ly);
                    if (row >= 0 && srcAt(row, s))
                    {   // row body: LOAD ONLY — quickload slots untouched, picker stays open
                        c.plugin->loadSource(s);
                        dirty = true;
                    }
                    break;
                }
                if (c.modal == 5)
                {
                    float lx, ly;
                    p5ui::pointToLayout((float)ev.xbutton.x, (float)ev.xbutton.y,
                                        (float)c.w, (float)c.h, lx, ly);
                    if (!p5ui::trim::inside(lx, ly)) { c.modal = 0; dirty = true; break; }
                    int row, col;
                    if (p5ui::trim::hitBar(lx, ly, &row, &col) && col >= 0)
                    {
                        c.trimDrag = 1; c.trimRow = row; c.trimCol = col;
                        applyTrimDrag(c, lx);
                        XGrabPointer(c.dpy, c.win, True,
                                     ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync,
                                     None, None, CurrentTime);
                        c.pointerGrabbed = true;
                        dirty = true;
                    }
                    break;                            // page is modal: swallow the press
                }
                if (c.modal == 7)                     // tuning file picker
                {
                    float lx, ly;
                    p5ui::pointToLayout((float)ev.xbutton.x, (float)ev.xbutton.y,
                                        (float)c.w, (float)c.h, lx, ly);
                    if (!p5ui::pick::inside(lx, ly)) { c.modal = 6; dirty = true; break; }
                    const int tab = p5ui::pick::hitTab(lx, ly);
                    if (tab >= 0) { c.tuneTab = tab; c.tunePage = 0; dirty = true; break; }
                    const int totalRows = 1 + (c.tuneTab ? (int)c.tuneFiles.size() : P5_STOCK_N);
                    const int pages = (totalRows + p5ui::pick::rows - 1) / p5ui::pick::rows;
                    const int pg = p5ui::pick::hitPage(lx, ly);
                    if (pg)
                    {   c.tunePage += pg;
                        if (c.tunePage < 0) c.tunePage = 0;
                        if (c.tunePage >= pages) c.tunePage = pages - 1;
                        dirty = true; break; }
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
                        {   applyStock(c, idx - 1); c.modal = 6; dirty = true; break; }
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
                        c.modal = 6; dirty = true; break;
                    }
                    break;
                }
                if (c.modal == 6)                     // microtuning editor page
                {
                    float lx, ly;
                    p5ui::pointToLayout((float)ev.xbutton.x, (float)ev.xbutton.y,
                                        (float)c.w, (float)c.h, lx, ly);
                    if (!p5ui::tune::inside(lx, ly)) { c.modal = 0; dirty = true; break; }
                    {   if (!c.wheelN)                        // first touch of the grid = adopt 12-TET
                        {   c.wheelN = 12;
                            for (int k = 1; k <= 12; k++) c.wheelC[k - 1] = 100.f * k;
                        }
                        const int wn = p5ui::tune::nodeAt(c.wheelC, c.wheelN, lx, ly);
                        if (wn >= 0)
                        {   c.wheelHot = wn;
                            const bool dbl = (wn == c.wheelLastIdx &&
                                              (unsigned)(ev.xbutton.time - c.wheelLastMs) < 450u);
                            c.wheelLastMs = (unsigned)ev.xbutton.time; c.wheelLastIdx = wn;
                            if (dbl)                          // double-click: snap this tone to equal
                            {   const float et = p5ui::tune::etCents(c.wheelC, c.wheelN);
                                const float nc = p5ui::tune::clampTone(c.wheelC, c.wheelN, wn, et * wn);
                                if (nc != c.wheelC[wn - 1])
                                {   c.wheelC[wn - 1] = nc; wheelCommit(c);
                                    std::fprintf(stderr, "[p5gui] wheel tone %d snapped to equal\n", wn); }
                                dirty = true; break;
                            }
                            c.wheelDragIdx = wn;
                            c.wheelDragC0 = c.wheelC[wn - 1];
                            c.wheelDragDist0 = std::hypot(lx - p5ui::tune::wheelCX, ly - p5ui::tune::wheelCY);
                            XGrabPointer(c.dpy, c.win, True,
                                         ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync,
                                         None, None, CurrentTime);
                            c.pointerGrabbed = true;
                            dirty = true; break;
                        }
                    }
                    const int ctl = p5ui::tune::hitControl(lx, ly);
                    if (ctl == 1) { c.modal = 7; c.tunePage = 0; c.tuneTab = 0; dirty = true; }
                    else if (ctl == 5)
                    {   c.modal = 8; c.modalLen = 0; c.modalBuf[0] = 0;
                        std::string seed(c.plugin->tuneName_);
                        if (!seed.empty() && seed.back() == '*') seed.pop_back();
                        while (!seed.empty() && seed.back() == ' ') seed.pop_back();
                        if (seed.empty()) seed = "my tuning";
                        std::strncpy(c.modalBuf, seed.c_str(), sizeof c.modalBuf - 1);
                        c.modalBuf[sizeof c.modalBuf - 1] = 0;
                        c.modalLen = (int)std::strlen(c.modalBuf);
                        XSetInputFocus(c.dpy, c.win, RevertToParent, CurrentTime);
                        dirty = true; }
                    else if (ctl == 6 || ctl == 7)
                    {   c.wheelRoot = (c.wheelRoot + (ctl == 7 ? 1 : -1) + 12) % 12;
                        wheelApply(c);
                        std::fprintf(stderr, "[p5gui] tuning root -> %d\n", c.wheelRoot);
                        dirty = true; }
                    else if (ctl == 2)
                    {   c.plugin->engine.setTuningDeviation(nullptr);
                        std::snprintf(c.plugin->tuneName_, sizeof c.plugin->tuneName_, "12-TET");
                        c.plugin->tuneToneCount_ = 0; c.wheelHot = -1;
                        c.wheelN = 12; c.wheelMask = 0x0fff; c.wheelWalk = 0;
                        for (int i = 0; i < 12; i++) c.wheelC[i] = 100.f * (i + 1);  // editable grid
                        std::fprintf(stderr, "[p5gui] tuning RESET to 12-TET\n");
                        dirty = true; }
                    else if (ctl >= 3)
                    {   c.tuneDrag = ctl; c.tuneFocus = ctl - 3; applyTuneDrag(c, lx, ev.xbutton.state);
                        XGrabPointer(c.dpy, c.win, True,
                                     ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync,
                                     None, None, CurrentTime);
                        c.pointerGrabbed = true; dirty = true; }
                    break;                            // page is modal: swallow the press
                }
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
                { actuate(c, c.pressBtn, ev.xkey.state); dirty = true; }  // LEDs flip NOW
                if (c.dragKnob >= 0)                        // gesture ends: tell the host the FINAL value
                { c.plugin->guiSync((clap_id)p5ui::kKnobs[c.dragKnob].pid); dirty = true; }
                c.dragKnob = -1; c.pressBtn = -1; c.trimDrag = 0; c.tuneDrag = 0;
                c.wheelDragIdx = 0;
                break;
            case MotionNotify:
                if (c.wheelDragIdx)
                {
                    float lx, ly;
                    p5ui::pointToLayout((float)ev.xmotion.x, (float)ev.xmotion.y,
                                        (float)c.w, (float)c.h, lx, ly);
                    applyWheelDrag(c, lx, ly, ev.xmotion.state);
                    dirty = true;
                    break;
                }
                if (c.tuneDrag)
                {
                    float lx, ly;
                    p5ui::pointToLayout((float)ev.xmotion.x, (float)ev.xmotion.y,
                                        (float)c.w, (float)c.h, lx, ly);
                    applyTuneDrag(c, lx, ev.xmotion.state);
                    dirty = true;
                    break;
                }
                if (c.trimDrag)
                {
                    float lx, ly;
                    p5ui::pointToLayout((float)ev.xmotion.x, (float)ev.xmotion.y,
                                        (float)c.w, (float)c.h, lx, ly);
                    applyTrimDrag(c, lx);
                    dirty = true;
                }
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
                    dirty = true;   // rendered once after the burst drains
                }
                break;
            case ClientMessage:
                if (ev.xclient.message_type == c.xembed) { /* EMBEDDED/FOCUS/CONFIGURE lifecycle */ }
                break;
            case KeyPress:
            {
                if (!c.modal) break;
                char xb[8]; KeySym ks = NoSymbol;
                const int n = XLookupString(&ev.xkey, xb, sizeof xb - 1, &ks, nullptr);
                if (ks == XK_Escape)
                {   if (c.modal == 3) c.modal = 1;
                    else if (c.modal == 9) c.modal = 8;
                    else if (c.modal == 8) c.modal = 6;
                    else c.modal = 0;
                    dirty = true; break; }
                if (ks == XK_BackSpace) { if (c.modalLen > 0) c.modalBuf[--c.modalLen] = 0; dirty = true; break; }
                if (ks == XK_Return || ks == XK_KP_Enter)
                {
                    c.modalBuf[c.modalLen] = 0;
                    if (c.modal == 1)
                    {
                        const std::string hit = p5store::foldCollision(c.modalBuf);
                        if (!hit.empty())     // faithful case, but "peace"==... -> confirm
                        {
                            std::snprintf(c.modalHit, sizeof c.modalHit, "%s", hit.c_str());
                            c.modal = 3; dirty = true; break;
                        }
                        c.plugin->savePatchNamed(c.modalBuf);
                        c.modal = 0; c.modalLen = 0;
                    }
                    else if (c.modal == 2) { c.plugin->assignSlotFromText(c.modalSlot, c.modalBuf); c.modal = 0; c.modalLen = 0; }
                    else if (c.modal == 3) { c.plugin->savePatchNamed(c.modalBuf); c.modal = 0; c.modalLen = 0; }
                    else if (c.modal == 8)
                    {   if (p5store::tuningFileExists(c.modalBuf))
                        {   std::snprintf(c.modalHit, sizeof c.modalHit, "%s", c.modalBuf);
                            c.modal = 9; dirty = true; break; }
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
                    dirty = true; break;
                }
                if (c.modal == 6)                     // tuning page: arrows adjust the focused range rail
                {
                    float up = (float)c.plugin->engine.bendRangeUpDefault();
                    float dn = (float)c.plugin->engine.bendRangeDownDefault();
                    const float d = (ev.xkey.state & ShiftMask) ? 1.0f : 0.1f;  // Shift = integer step
                    bool used = true;
                    if (ks == XK_Left) c.tuneFocus = 0;
                    else if (ks == XK_Right) c.tuneFocus = 1;
                    else if (ks == XK_Up || ks == XK_KP_Up)   { if (c.tuneFocus) dn += d; else up += d; }
                    else if (ks == XK_Down || ks == XK_KP_Down) { if (c.tuneFocus) dn -= d; else up -= d; }
                    else used = false;
                    if (used)
                    {   auto clampQ = [](float v) { v = v < 0.f ? 0.f : v > 24.f ? 24.f : v;
                                                    return (float)((int)(v * 10.0f + 0.5f)) / 10.0f; };
                        c.plugin->engine.setBendRangeDefault(clampQ(up), clampQ(dn));
                        dirty = true; break;
                    }
                }
                if (n > 0)
                {
                    const char ch = xb[0];
                    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == ' ' || ch == '.' || ch == '_' || ch == '-')
                    {   // faithful case: XLookupString has already applied shift
                        if (c.modalLen < (int)sizeof c.modalBuf - 1)
                            c.modalBuf[c.modalLen++] = ch;
                        dirty = true;
                    }
                }
                break;
            }
            default:
                break;
        }
    }
    // The whole X backlog is applied and drawn exactly once. Rendering per
    // motion event serialized us on glXSwapBuffers: after a fast drag the
    // stale backlog kept replaying positions (and their blocking swaps) long
    // after release — that was the audible "trail", ours, not the host's.
    if (dirty) render(c);
    // (shrink-negotiation removed: surplus height is now DRAWN chassis, so
    // asking the host to shrink it would fight the ancestor-grow request)
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
    c.parent = parent;                       // pump/mirror/grow all key off this
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
                     StructureNotifyMask | KeyPressMask;   // ClientMessage needs no mask
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
    XStoreName(c.dpy, c.win, "Muhammad");
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
    classHint.res_name = const_cast<char *>("muhammad");
    classHint.res_class = const_cast<char *>("Muhammad");
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
