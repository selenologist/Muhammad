// SPDX-License-Identifier: GPL-3.0-only
// Control path of the Prophet engine: the voice-allocation and modulation work
// of melodee firmware/src/voice.c (trk_note_on/off, voice_alloc, voice_start,
// mono_play, track_render_audio) and the engine's part-level work of
// eng_prophet_test.c (p5_block, the p5_render parameter math, p5_post).
//
// CONTROL RATE: the firmware's 32-sample control tick is gone by owner
// decision (Oct 2026). Every time-varying control - the two envelopes, the
// VCA, pitch (bend, glide, drift), the LFO rate/depth/phase and the knob
// slew - advances once per HOST SAMPLE inside process(), independent of
// where the host chops its buffers or where events land. The old block-lerp
// law (tick once per call, interpolate i/32) snapped the ramp endpoint at
// every partial block - the occasional click at note-on - and the per-call
// knob update with uint8 value_ stair-quantized the LFO and every slewed
// parameter. The kernels now read per-sample control arrays materialized
// here, so scalar and wide stay bit-identical to EACH OTHER (the A/B still
// certifies that) while the firmware dump comparison is retired as
// superseded - the same kind of deliberate deviation as the OS decimator.
// Static per-note math (patch bytes -> coefficients) stays at call rate on
// block-start knob snapshots: a knob cannot move more than 0.7 ms of its own
// 10 ms slew between calls, and nothing per-sample reads stale bytes.
#include <cmath>
#include <cstring>    // IWYU: memcpy lives here, not in the header chain
#include "p5_engine.hpp"
#include "p5_math.hpp"

namespace p5 {

static const int V_POLY = 0, V_MONO = 1, V_LEGATO = 2, V_UNISON = 3;

static inline float f_max(float a, float b) { return a > b ? a : b; }

// ---- continuous table lookups (the byte stair is gone) ----------------------
static float lutF(const float *t, float x)
{
    x = f_clamp(x, 0.0f, 127.0f);
    const uint32_t i = (uint32_t)x;
    if (i >= 127u) return t[127];
    const float f = x - (float)i;
    return t[i] + (t[i + 1u] - t[i]) * f;
}
static float lutU(const uint32_t *t, float x)
{
    x = f_clamp(x, 0.0f, 127.0f);
    const uint32_t i = (uint32_t)x;
    if (i >= 127u) return (float)t[127];
    const float f = x - (float)i;
    return (float)t[i] + ((float)t[i + 1u] - (float)t[i]) * f;
}

// ---- per-sample envelope law (the old envTickEngine, /32 per tick) ----------
static EnvRates envRatesF(const float *q, int a, int d, int s, int r, int relOn,
                         double srScale, int ssm, float variation)
{
    const float sc = (float)(srScale * (1.0 / 32.0)) * (1.0f + variation * (1.0f / 32768.0f));
    EnvRates L;
    L.ssm  = ssm;
    L.atk  = lutF(PW_P5_ENV_ATK, q[a]) * sc;
    L.tgt  = q[s] * (1.0f / 127.0f);
    L.expD = f_min(lutF(PW_P5_ENV_EXP, q[d]) * sc, 1.0f);
    const float rB = relOn ? q[r] : 16.0f;
    L.expR = f_min(lutF(PW_P5_ENV_EXP, rB) * sc, 1.0f);
    L.ssmD = lutF(PW_P5_ENV_SSM, q[d]) * sc;
    L.ssmR = lutF(PW_P5_ENV_SSM, rB) * sc;
    return L;
}

static inline void envTick1(Env &e, const EnvRates &L, int gate, float cut)
{
    if (!gate && e.stage != 0) e.stage = 3;
    if (e.stage == 1) {
        e.value += L.atk;
        if (e.value >= 1.0f) { e.value = 1.0f; e.stage = 2; }
    } else if (L.ssm && e.stage >= 2) {
        const float tgt = e.stage == 2 ? L.tgt : 0.0f;
        const float step = e.stage == 2 ? L.ssmD : L.ssmR;
        e.value += f_clamp(tgt - e.value, -step, step);
    } else if (e.stage == 2) {
        e.value += (L.tgt - e.value) * L.expD;
    } else if (e.stage == 3) {
        e.value -= e.value * L.expR;
    }
    if (e.stage == 3 && e.value < cut) { e.value = 0.0f; e.stage = 0; }
}

Engine::Engine(KernelMode mode) : mode_(mode)
{
    buildTpt();          // S12: tpt_ must never point at the 128-entry dump
                         // table before activate() (filterCore indexes past it)
    Patch p;
    patchInit(p);
    setPatch(p);
    silenceReset();
}

const char *Engine::simdPath() const
{
#ifdef P5_HAVE_AVX2
    return simdK_ == p5KernelSimdAVX2 ? "avx2" : "sse2";
#elif defined(__ARM_NEON)
    return "neon";
#else
    return "sse2";
#endif
}

void Engine::activate(double sampleRate)
{
    rowsSettled_ = false;
#ifdef P5_HAVE_AVX2
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
        simdK_ = p5KernelSimdAVX2;   // wide kernel at true AVX2, still contract=off
#endif
    srScale_ = sampleRate > 0.0 ? 44100.0 / sampleRate : 1.0;
    hostSr_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    // the old per-call quarter-step bend slew (exact landing, ~2.8 ms) as a
    // per-sample factor: one 32-step of the same geometric decay per sample
    smoothBend_ = 1.0 - std::exp(-srScale_ * (std::log(4.0 / 3.0) / 32.0));
    buildTpt();
    if (sampleRate == 44100.0) {
        dcK_ = DC_K_DEFAULT;            // the firmware's P5_DC_K literal, bit for bit
    } else {
        // the output coupling capacitor, about 5 Hz: k = (1 - 2 pi 5 / sr) << 30
        const double PI = 3.14159265358979323846;
        double k = (1.0 - 2.0 * PI * 5.0 / sampleRate) * 1073741824.0;
        dcK_ = (uint32_t)(k > 0.0 ? k : 0.0);
    }
    dcX1_ = dcY1_ = 0;
    // E9: sample-rate-invariant time constants, computed ONCE per activation
    // (the per-sample path only multiplies by these). At 44100 every value
    // equals the old literal, so the certified renders are bit-identical.
    const double k = srScale_;
    fadeDec_     = (float)(k * (1.0 / 32.0));
    vcaStep_     = fadeDec_;
    glideFloor_  = fadeDec_;
    // At the reference rate keep the certified literals BIT FOR BIT (the pow
    // of 0.9995^(1/32) rounds a hair off the transcribed decimal); away from
    // it, follow the same laws scaled.
    driftLeak_   = k == 1.0 ? 0.9999843753f : (float)std::pow(0.9995, k / 32.0);
    driftSig_    = k == 1.0 ? 0.019445f     : (float)(0.11 * std::sqrt(k / 32.0));
    atS_ = 0.f; brightS_ = (float)brightness_; mwS_ = (float)mw_;
    // Live controller/bend PERFORMANCE state resets with activation; the
    // PERSISTENT state (microtuning, bend-range defaults, per-voice trims,
    // the patch) does NOT - review C3. The plugin re-pushes its authoritative
    // copy after activate anyway; the engine must not be the one to forget.
    for (int i = 0; i < 16; i++) { bendRaw_[i] = 0; bendF_[i] = 0; bendRawSeen_[i] = 0; bendRngSeen_[i] = 0;
                                   rUp_[i] = rDn_[i] = -1; }
    hbReset(hbMixL_); hbReset(hbMixR_);
}

// Rebuild the cutoff->g table for the internal rate (os_ * hostSr_). The
// firmware table is tan(pi*fc_i/44100)/(1+tan(pi*fc_i/44100)) per index; the
// firmware's own P5_OVERSAMPLE=2 variant is the same expression at 2*FS.
// At (44100, 1x) we use the dumped table verbatim so the scalar/wide A/B
// never sees a recomputed value. (Inverting g -> tan -> rescale -> g keeps
// every index's cutoff in exact Hertz at any host sample rate.)
void Engine::buildTpt()
{
    if (os_ == 1 && hostSr_ == 44100.0)
        for (int i = 0; i < 128; i++) tptGen_[i] = PW_P5_TPT_G[i];  // verbatim firmware values
    else
    {
        const double scale = 44100.0 / ((double)os_ * hostSr_);
        // E1: atan(t)*scale can pass pi/2 when the host rate is below the
        // table's reference (22.05k/24k at 1x): tan flips NEGATIVE there and
        // the old one-sided guard let it through, giving g in ~[1.7, 7] -
        // an unstable ladder that made INIT oscillate at Nyquist. Clamp the
        // ANGLE to the same 98%-of-Nyquist (max 21 kHz) cap the headroom
        // section below uses. At scale <= 1 the clamp never fires, so 44.1k/
        // 48k renders are untouched bit for bit.
        const double amax = 3.14159265358979318 * (((double)os_ * hostSr_) * 0.49 < 21000.0
                                                   ? 0.49 : 21000.0 / ((double)os_ * hostSr_));
        for (int i = 0; i < 128; i++) {
            const double g0 = PW_P5_TPT_G[i];
            const double t = g0 / (1.0 - g0);                 // tan(pi fc_i / 44100)
            double ang = std::atan(t) * scale;
            if (ang > amax) ang = amax;
            double tn = std::tan(ang);
            if (!(tn < 1.0e6)) tn = 1.0e6;                    // never at/past Nyquist in practice
            tptGen_[i] = (float)(tn / (1.0 + tn));
        }
    }
    // Ultrasonic headroom: the generator's law is fc(u) = 30 * (16000/30)^(u/127) Hz;
    // continue it past 127, flattening at min(21 kHz, 98% internal Nyquist).
    const double srInt = (double)os_ * hostSr_;
    const double fcap = srInt * 0.49 < 21000.0 ? srInt * 0.49 : 21000.0;
    for (int u = 128; u <= PW_TPT_TOP; u++)
    {
        double f = 30.0 * std::pow(16000.0 / 30.0, (double)u / 127.0);
        if (f > fcap) f = fcap;
        double t = std::tan(3.14159265358979318 * f / srInt);
        if (!(t < 1.0e6)) t = 1.0e6;
        tptGen_[u] = (float)(t / (1.0 + t));
    }
    tpt_ = tptGen_;
}

void Engine::setSmoothing(bool on)
{
    smooth_ = on;
    rowsSettled_ = false;
    if (on) for (uint32_t k = 0; k < 55u; k++) fval_[k] = (float)valueOf((int)k);
}

void Engine::setTuneCents(int cents)
{
    if (cents < -100) cents = -100;
    if (cents > 100) cents = 100;
    tuneCents_ = cents;
    tuneUnits_ = (int)std::lround(cents * 16.0 / 100.0);   // 16 pitch units = 1 semitone
}

void Engine::setReleaseCut(float lin)
{
    if (lin < 1.0e-9f) lin = 1.0e-9f;
    envCut_ = lin;
}

void Engine::setOverSample(int os)
{
    const int os2 = (os == 2) ? 2 : 1;
    if (os2 == os_) return;
    os_ = os2;
    buildTpt();
    hbReset(hbMixL_); hbReset(hbMixR_);
}

// p5_track_accept: native performance controls become the track's allocator settings
void Engine::trackAccept()
{
    voiceMode_ = patch_.raw[P5_UNISON] ? V_UNISON : V_POLY;
    prio_ = valueOf(P5_RETRIGGER) < 2 ? 1 : 0;
    detune_ = i_clamp((int32_t)patch_.raw[P5_UNISON_DETUNE], 0, 7) * 127 / 7;
}

void Engine::setPatch(const Patch &p)
{
    patch_ = p;
    rowsSettled_ = false;
    // E2: a patch load is a NEW SOUND, not a morph. With smoothing on, the
    // old knob state slewed to the new targets over ~100 ms - a note arriving
    // with a sequenced program change played the OLD cutoff/levels and got a
    // coarse-pitch glissando on top. Snap the slewed state to the new patch
    // (bit-exact path rewrites fval_ every call anyway - no behaviour change).
    if (smooth_)
        for (uint32_t k = 0; k < 55u; k++) fval_[k] = (float)valueOf((int)k);
    trackAccept();
    wheelTaken_ = false;
    wheelSeed();
    bendRngDirty_ = true;   // E10: the wire range byte comes in with the patch
    monoNote_ = 0; nmono_ = 0;
    // the part's shared state starts afresh with a new patch (the p5_ready path)
    lfoPhaseD_ = 0; modNoise_ = 0; lastPitch_ = 0;
    for (int i = 0; i < MAXV; i++)
    {   // E6: a voice that was SOUNDING gets the 32-sample death fade instead
        // of a one-sample hard cut (audible click on every program change).
        // lastPcm holds the lane's last rendered block, which is what fades.
        if (lanes_[i].v.active) { lanes_[i].v.active = 0; lanes_[i].v.gate = 0;
                                  lanes_[i].fadeT = 1.0f; }
        else { lanes_[i].fadeT = 0.0f; lanes_[i].lastPcm = 0; }
    }
    live_ = 0;
}

void Engine::loadFactory(uint32_t k)
{
    Patch p;
    if (k >= PW_FACTORY_N) { patchInit(p); setPatch(p); return; }
    std::memset(&p, 0, sizeof p);
    std::memcpy(p.raw, PW_FACTORY_RAW[k], WIRE_SIZE);
    p.raw[P5_FEEDBACK] = 0;
    p.size = RAW_MAX; p.model = 0x32; p.command = 2;
    p.group = (uint8_t)(k / 40u); p.program = (uint8_t)(k % 40u);
    setPatch(p);
}

int Engine::slotCount() const
{
    // p5_cap: the unison count inside the poly cap, else the cap itself
    int mode = voiceModeOverride_ >= 0 ? voiceModeOverride_ : voiceMode_;
    if (mode == V_UNISON) {
        int c = patch_.raw[P5_UNISON_COUNT] ? patch_.raw[P5_UNISON_COUNT] : 5;
        return i_clamp(c, 1, poly_);
    }
    return poly_;
}

void Engine::silenceReset()
{
    rowsSettled_ = false;
    // C4/H3: a host reset must silence and restart PERFORMANCE state only.
    // It used to memset whole Lanes, which zeroed panG (PAN WIDTH > 0 made
    // the plugin SILENT until the width knob was touched again - panDirty_
    // was never set so process() never recomputed the gains) and flattened
    // the per-voice V/oct trims, which are project state saved in the blob.
    // Field-by-field now: voices, envelopes, glide, drift, VCA/fade, channel.
    for (int i = 0; i < MAXV; i++) {
        Lane &l = lanes_[i];
        l.v = {};
        l.fenv = {}; l.aenv = {}; l.envDrift = 0;
        l.glideF = 0.f; l.curF = 0.f;
        l.vca = 0.f; l.fadeT = 0.f;
        l.wP = l.wF = l.wA = 0.f;
        l.ch = 0;
        l.lastPcm = 0;
        l.ampComp = 1.0f;
        l.p = {};
        phaseA_[i] = phaseB_[i] = noise_[i] = 0;
        fbState_[i] = 0.f;
        for (int j = 0; j < 4; j++) z_[j][i] = 0.0f;
        triangle_[i] = 0.0f; syncTail_[i] = 0.0f;
        l.vph[0] = l.vph[1] = 0;
        wSt_[i] = (uint32_t)(0x9E3779B9u + i * 0x85EBCA6Bu);
    }
    panDirty_ = true;                       // recompute panG for the next block
    lfoPhaseD_ = 0; lastPitch_ = 0; modNoise_ = 0; dcX1_ = dcY1_ = 0;
    for (int i = 0; i < 16; i++) bendF_[i] = 0.0f;
    // H3: snap the knob slews to their CURRENT targets - they were zeroed,
    // so every knob swept up from 0 (~100 ms filter/level sweep and a
    // semitone glissando on the first notes after every transport jump).
    for (uint32_t k = 0; k < 55u; k++) fval_[k] = (float)valueOf((int)k);
    atS_ = 0.f; brightS_ = 0.f;             // smoothed controllers start silent/clean
    hbReset(hbMixL_); hbReset(hbMixR_);
    live_ = 0; monoNote_ = 0; nmono_ = 0;
    vage_ = 0;   // (the firmware's vage clock is process-global; the A/B harness resets it per scenario)
}

// --------------------------------------------------------------- note-on ----
// p5_note_on: voiceWas = voice_start's 0 fresh / 1 released / 2 held
void Engine::synthNoteOn(int lane, int voiceWas)
{
    const uint8_t *p = patch_.raw;
    Lane &l = lanes_[lane];
    if (!voiceWas) {
        std::memset(&l.fenv, 0, sizeof l.fenv);
        std::memset(&l.aenv, 0, sizeof l.aenv);
        l.glideF = 0.0f; l.lastPcm = 0;
        l.envDrift = 0;
        fbState_[lane] = 0.f;                          // fresh voice: loop silent
        phaseA_[lane] = l.vph[0];
        phaseB_[lane] = l.vph[1];
        for (int j = 0; j < 4; j++) z_[j][lane] = 0.0f;
        triangle_[lane] = 1.0f;
        syncTail_[lane] = 0.0f;
        // no decimator reset: the mix decimator is shared by all voices and
        // must keep running through note boundaries (per-voice reset used to
        // re-stamp the loop phase - part of the high-feedback gremlins)
        // Thermal excitation lets resonance start with the oscillator mixer shut,
        // as native self-oscillating factory programs require: seed the filter.
        if (p[P5_RESONANCE] >= 110u && !p[P5_LEVEL_A] && !p[P5_LEVEL_B] && !p[P5_NOISE]) {
            z_[0][lane] = 0.5f; z_[2][lane] = -0.5f;
        }
        noise_[lane] = (uint32_t)(0x5A1793 + (int32_t)l.v.age * 17 + lane * 101);
    }
    if (!voiceWas) l.glideF = lastPitch_ ? (float)lastPitch_ : (float)l.v.pitch16;
    lastPitch_ = l.v.pitch16;
    l.fenv.stage = l.aenv.stage = 1;
    // VINTAGE now means DRIFT: the static per-note roulette is retired. The
    // OU walks are continuous across notes, like a hot board (per-sample).
    l.envDrift = 0;
    int32_t *st = (int32_t *)&noise_[lane];
    // The firmware burned 4 LCG draws here for the vintage roulette. The
    // draws are gone but the STREAM must not shift: the LOW_B phase seed and
    // every voice-noise sample after it must stay phase-locked with the
    // reference (port lesson: transcribe the quirk, then deviate on purpose).
    for (int burn = 0; burn < 4; burn++) fastNoise32(st);
    // The oscillators free-run, so a key finds Lo Freq B anywhere in its cycle.
    if (!voiceWas && p[P5_LOW_B]) phaseB_[lane] = fastNoise32(st);
}

void Engine::voiceStart(int lane, int note, int vel, uint8_t mvel, int *voiceWas, int *sounding,
                        float comp)
{
    Lane &l = lanes_[lane];
    *voiceWas = !l.v.active ? 0 : (l.v.gate ? 2 : 1);
    *sounding = l.v.active;
    l.v.note = (uint8_t)note;
    l.v.vel = (uint8_t)vel;
    l.v.gate = 1;
    l.v.active = 1;
    l.v.age = ++vage_;
    l.ch = curCh_;
    l.v.pitch16 = note * 16 + tuneUnits_ + (tuningActive_ ? tuneDev_[i_clamp(note, 0, 127)] : 0);
    l.v.pitchCur = l.v.pitch16;                   // glide_set: the track's glide is off
    l.curF = (float)l.v.pitch16;
    l.v.mvel = mvel;                              // t->m_vel (mod.c: the note's VEL)
    l.ampComp = comp;                             // E4: 1/sqrt(unison count) level compensation
    if (!*sounding) { l.vca = 0.0f; l.fadeT = 0.0f; }   // the VCA onset ramp restarts
}

void Engine::polyNoteOn(int note, int vel)
{
    // voice_alloc with P_ALLOC = 1 (REUSE: the free voice whose last pitch is closest)
    const int np = slotCount();
    int lane = -1;
    for (int i = 0; i < np; i++)                       // MPE: reuse only within one channel
        if (lanes_[i].v.active && lanes_[i].v.note == note && lanes_[i].ch == curCh_) { lane = i; break; }
    if (lane < 0) {
        int32_t target = note * 16, bd = 0x7FFFFFFF;
        for (int i = 0; i < np; i++) {                  // the nearest last pitch, never played: last
            if (lanes_[i].v.active) continue;
            int32_t d = lanes_[i].v.pitchCur - target; if (d < 0) d = -d;
            if (!lanes_[i].v.pitchCur) d = 0x7FFFFFF0;
            if (d < bd) { bd = d; lane = i; }
        }
        if (lane < 0) {                                 // steal: oldest released, then oldest held, bass protected
            int low = -1;
            for (int i = 0; i < np; i++)
                if (lanes_[i].v.active && lanes_[i].v.gate &&
                    (low < 0 || lanes_[i].v.note < lanes_[low].v.note)) low = i;
            for (int i = 0; i < np; i++)
                if (lanes_[i].v.active && !lanes_[i].v.gate &&
                    (lane < 0 || lanes_[i].v.age < lanes_[lane].v.age)) lane = i;
            if (lane < 0)
                for (int i = 0; i < np; i++)
                    if (lanes_[i].v.active && i != low &&
                        (lane < 0 || lanes_[i].v.age < lanes_[lane].v.age)) lane = i;
            if (lane < 0) lane = 0;
        }
    }
    int voiceWas, sounding;
    voiceStart(lane, note, vel, (uint8_t)vel, &voiceWas, &sounding);
    lanes_[lane].v.fine = 0;
    synthNoteOn(lane, voiceWas);
    monoNote_ = 0; nmono_ = 0;
}

int Engine::monoPick() const
{
    // the note the mono stack asks for (PRIO: 0 LAST, 1 LOW, 2 HIGH), 0 = none
    if (!nmono_) return 0;
    unsigned n = monoStack_[nmono_ - 1];
    for (uint32_t i = 0; i < nmono_; i++) {
        if (prio_ == 1 && monoStack_[i] < n) n = monoStack_[i];
        if (prio_ == 2 && monoStack_[i] > n) n = monoStack_[i];
    }
    return (int)n;
}

void Engine::monoNoteOn(int note, int vel)
{
    // mono_play: one note over the stack's voices (UNISON spreads over the cap)
    const int mode = voiceModeOverride_ >= 0 ? voiceModeOverride_ : voiceMode_;
    const int np = slotCount();
    {   // mono_remove(note), then push (a full stack drops its oldest)
        uint32_t k = 0;
        for (uint32_t j = 0; j < nmono_; j++) if (monoStack_[j] != note) monoStack_[k++] = monoStack_[j];
        nmono_ = k;
        if (nmono_ >= (uint32_t)MAXV) {                 // mono_remove(mono_stack[0])
            for (uint32_t j = 1; j < nmono_; j++) monoStack_[j - 1] = monoStack_[j];
            nmono_--;
        }
        monoStack_[nmono_++] = (uint8_t)note;
    }
    const int sounding = monoNote_ != 0 && lanes_[0].v.active && lanes_[0].v.gate;
    const int want = monoPick();
    // E5: with the RET (bit 0 of the retrigger byte) law, re-striking the
    // key that ALREADY has the voice must retrigger, not fall through the
    // "winner unchanged" early return. A different key losing the priority
    // contest still returns (the sounding note is not what was pressed).
    const int retrBit = patch_.raw[P5_RETRIGGER] & 1u;
    if (sounding && want == monoNote_ && !(retrBit && note == want)) return;
    const int retrigger = mode == V_MONO ? 1
                        : (!sounding || (want == monoNote_ && retrBit));
    const int nv = mode == V_UNISON ? np : 1;
    // E4: unison voices SUM, so each must play quieter or N voices arrive
    // +14..18 dB hot. The firmware's 36/100 for a full 8-voice stack IS the
    // 1/sqrt(nv) power law (8 -> 0.354); the old code applied its scale to
    // v.vel, which never reaches the audio - dead compensation. Apply
    // 1/sqrt(nv) to the amp path per voice (filter velocity untouched).
    const float comp = nv > 1 ? (float)(1.0 / std::sqrt((double)nv)) : 1.0f;
    for (int i = 0; i < nv; i++) {
        Lane &l = lanes_[i];
        int32_t k = 2 * i - (nv - 1);                   // -7..7
        if (nv > 1 && nv != NPOLY_) k = k * (NPOLY_ - 1) / (nv - 1);
        l.v.fine = nv > 1 ? k * detune_ * 56 / 889 : 0; // up to ~+-40 cents
        if (retrigger || !l.v.active || !l.v.gate) {
            int voiceWas, snd;
            // E4: seed the spread phases BEFORE the fresh voice copies vph
            // into the running phase (synthNoteOn) - the old order seeded
            // after, so the FIRST unison note played phase-coherent voices.
            if (nv > 1 && i > 0) {                      // random start phases: evenly spread, they cancel
                unisonSeed_ = unisonSeed_ * 1664525u + 1013904223u;
                l.vph[0] += unisonSeed_;
            }
            // E3: the RAW velocity goes to the voice; the fallback path in
            // noteOff re-reads lanes_[0].v.vel as the next note's velocity,
            // and the old pre-scaled storage halved it (36/100 at 8 voices)
            // every time a held key released onto this one.
            voiceStart(i, want, vel, (uint8_t)vel, &voiceWas, &snd, comp);
            synthNoteOn(i, voiceWas);
        } else {
            l.v.note = (uint8_t)want;                   // legato: new pitch, same envelope
            l.v.pitch16 = want * 16 + tuneUnits_ + (tuningActive_ ? tuneDev_[i_clamp(want, 0, 127)] : 0);
            l.v.pitchCur = l.v.pitch16;
            l.curF = (float)l.v.pitch16;
            l.v.mvel = (uint8_t)vel;
            if (vel > l.v.vel && nv == 1) l.v.vel = (uint8_t)vel;
            if (patch_.raw[P5_RETRIGGER] & 1u) l.fenv.stage = l.aenv.stage = 1;   // p5_legato
        }
    }
    monoNote_ = want;
}

void Engine::noteOn(int note, int vel, int ch)
{
    curCh_ = ch & 15;
    if (note < 0 || note > 127) return;
    vel = i_clamp(vel, 1, 127);
    const int mode = voiceModeOverride_ >= 0 ? voiceModeOverride_ : voiceMode_;
    if (mode == V_POLY) polyNoteOn(note, vel);
    else monoNoteOn(note, vel);
}

void Engine::noteOff(int note, int ch)
{
    const int mode = voiceModeOverride_ >= 0 ? voiceModeOverride_ : voiceMode_;
    const int np = slotCount();
    {
        uint32_t k = 0;
        for (uint32_t j = 0; j < nmono_; j++) if (monoStack_[j] != note) monoStack_[k++] = monoStack_[j];
        nmono_ = k;
    }
    if (mode != V_POLY) {
        const int nv = mode == V_UNISON ? np : 1;
        if (monoNote_ == note) {
            const int next = monoPick();
            if (next) {
                // trk_note_on wants=next, vel from v[0]: the mono path again, legato
                const int v0 = lanes_[0].v.vel;
                monoNoteOn(next, v0);
            } else {
                for (int i = 0; i < nv; i++)
                    if (lanes_[i].v.gate) lanes_[i].v.gate = 0;
                monoNote_ = 0;
            }
        }
    }
    for (int i = 0; i < MAXV; i++)                      // a fallback already has its new pitch
        if (lanes_[i].v.note == note && lanes_[i].v.gate &&
            (ch < 0 || lanes_[i].ch == ch)) {           // MPE: same key, other channel survives
            lanes_[i].v.gate = 0;
        }
}

void Engine::allNotesOff()
{
    for (int i = 0; i < MAXV; i++)
        lanes_[i].v.gate = 0;
    nmono_ = 0; monoNote_ = 0;
}

void Engine::setBendRangeSemitones(int ch, double up, double down)
{
    const int q = (int)(up * 256.0 + (up >= 0 ? 0.5 : -0.5));
    const int r = (int)(down * 256.0 + (down >= 0 ? 0.5 : -0.5));
    for (int i = 0; i < 16; i++)
        if (ch < 0 || i == ch) { rUp_[i] = (int16_t)(q < 0 ? 0 : q > 24 * 256 ? 24 * 256 : q);
                                 rDn_[i] = (int16_t)(r < 0 ? 0 : r > 24 * 256 ? 24 * 256 : r); }
    bendRngDirty_ = true;                                  // E10
}
void Engine::setBendRangeDefault(double up, double down)
{
    rUpDef_ = (int16_t)(up * 256.0 + 0.5);
    rDnDef_ = (int16_t)(down * 256.0 + 0.5);
    bendRngDirty_ = true;                                  // E10
}

void Engine::setTuningDeviation(const int16_t *dev16)
{
    bool any = false;
    for (int k = 0; k < 128; k++)
    {
        const int v = dev16 ? dev16[k] : 0;
        tuneDev_[k] = (int16_t)(v < -512 ? -512 : v > 512 ? 512 : v);
        if (tuneDev_[k]) any = true;
    }
    tuningActive_ = any;
}

void Engine::setVoiceTrim(int lane, int osc, int scale, int offset)
{
    if (lane < 0 || lane >= MAXV || osc < 0 || osc > 1) return;
    Lane &l = lanes_[lane];
    l.trimS[osc] = (int16_t)(scale < 0 ? 0 : scale > 127 ? 127 : scale);
    l.trimO[osc] = (int16_t)(offset < 0 ? 0 : offset > 127 ? 127 : offset);
}
void Engine::voiceTrim(int lane, int osc, int *scale, int *offset) const
{
    *scale = lanes_[lane].trimS[osc]; *offset = lanes_[lane].trimO[osc];
}

// ---------------------------------------------------------- per-sample pitch --
// The continuous centsInc: p16f carries the live pitch stack in 1/16-steps
// (float), ctF the drift cents, fineF the fine term (v.fine + bend residual).
// The integer skeleton mirrors centsInc() of dsp.c exactly; only the inputs
// became continuous, so bend and glide slide instead of stepping per block.
static uint32_t incAt(float p16f, float ctF, float fineF, double srScale, int os)
{
    const int32_t ct16 = (int32_t)(ctF * 16.0f);
    const int32_t d16 = ct16 / 100;
    const float rem = (float)ct16 - (float)d16 * 100.0f;
    const uint32_t base = pitchInc((uint32_t)i_clamp((int32_t)p16f + d16, 0, 2047));
    const int32_t add = (int32_t)((float)(int32_t)(base >> 12) * (rem * (2367.0f / 16000.0f) + fineF));
    uint32_t ia = base + (uint32_t)add;
    if (srScale != 1.0)
    {   // S12: below ~13 kHz host rate the scaled increment exceeds uint32 -
        // an out-of-range double->uint conversion is UB. Saturate; near-
        // ultrasonic oscillators are silent through the BLEP anyway.
        const double s = (double)ia * srScale;
        ia = s < 4294967296.0 ? (uint32_t)s : 4294967295u;
    }
    if (os != 1) ia /= (uint32_t)os;
    return ia;
}

// Per-oscillator V/oct calibration, 1/16-semitone domain, slope pivoted at C4
// like a scope trim. Applied at the oscillator increment read, AFTER the shared
// glide/CV bus — which is exactly the hardware topology: each osc's own trim
// pot shapes its own control voltage. Continuous with the pitch it shapes.
float Engine::applyTrim(const Lane &l, float p16, int osc)
{
    const float sc = (float)(l.trimS[osc] - 64), of = (float)(l.trimO[osc] - 64);
    const float pt = p16 + of + (p16 - 960.0f) * sc * (1.0f / 1024.0f);
    return pt < 0.0f ? 0.0f : pt > 2047.0f ? 2047.0f : pt;
}

// one host sample of the part-wide parameter math, from the LIVE knob state.
// Everything a slewed knob can reach is here, so no parameter ever steps on
// a call boundary: cutoff, resonance, depths, widths, levels, coarse tuning,
// the glide step and both envelope laws.
void Engine::partSample(uint32_t i)
{
    const uint8_t *p = patch_.raw;
    const float *q = fval_;
    const int curtis = !!p[P5_FILTER_REV];
    // E11: smoothed controller reads (equal to the raw values when smooth_
    // is off, so the bit-exact path is untouched).
    const float c0 = q[P5_CUTOFF] + brightS_;   // Rev4 CC74 Brightness adds to cutoff
    float cut = 1618.0f + c0 * 236.0f
              + (curtis ? 0.0f : (127.0f - c0 > 66.0f ? 66.0f : 127.0f - c0) * 38.0f)
              + (float)e_[0] * 256.0f;
    if (p[P5_PRESS_FILTER]) cut += atS_ * 96.0f;
    const float resonance = f_clamp(q[P5_RESONANCE] + (float)e_[1], 0.0f, 127.0f);
    ppS_[PP_CUT][i]    = cut / 256.0f;
    ppS_[PP_K][i]      = resonance * (float)(curtis ? 19200 : 18800) / 127.0f / 4096.0f;
    ppS_[PP_FG][i]     = (32767.0f - resonance * (float)(curtis ? 100 : 45)) / 32768.0f;
    ppS_[PP_ENVAMT][i] = q[P5_ENV_FILTER] * (165.0f / 256.0f) * polEnv_;
    ppS_[PP_PAENV][i]  = q[P5_POLY_ENV] * (85.0f / 4096.0f) * polPoly_;
    ppS_[PP_PAB][i]    = q[P5_POLY_B] * (35.0f / 4096.0f) * polPolyB_;
    ppS_[PP_PFENV][i]  = q[P5_POLY_ENV] * (226.0f / 256.0f) * polPoly_;
    ppS_[PP_PFB][i]    = q[P5_POLY_B] * (226.0f / 256.0f) * polPolyB_;
    ppS_[PP_PWENV][i]  = q[P5_POLY_ENV] * (64.0f / 32768.0f) * polPoly_;
    ppS_[PP_PWB][i]    = q[P5_POLY_B] * (258.0f / 32768.0f) * polPolyB_;
    ppS_[PP_PWMA][i]   = (q[P5_PW_A] * 258.0f + (float)e_[4] * 256.0f) / 32768.0f;
    ppS_[PP_PWMB][i]   = (q[P5_PW_B] * 258.0f + (float)e_[5] * 256.0f) / 32768.0f;
    ppS_[PP_LA][i]     = f_clamp(q[P5_LEVEL_A] + (float)e_[6], 0.0f, 127.0f) * (258.0f / 32768.0f);
    ppS_[PP_LB][i]     = f_clamp(q[P5_LEVEL_B] + (float)e_[7], 0.0f, 127.0f) * (258.0f / 32768.0f);
    ppS_[PP_LN][i]     = q[P5_NOISE] * (258.0f / 32768.0f);
    // FEEDBACK: VCA -> filter input, per voice, like plugging the output
    // jack into the external input. The tap keeps its real envelope depth
    // (a quiet voice feeds back quietly - the hardware behaves exactly so);
    // the byte calibrates the cable: 3/127 puts FULL envelope sustain at
    // byte 127 at the edge of loop unity - the ladder sings and the
    // pre-ladder softclip distorts, which is the whole point at extreme
    // settings. Mid bytes sing less; byte 0 adds an exact +-0 (bit-exact).
    // Bits: 0..6 = cable amount, 7 = POLARITY (right-click on the knob; the
    // sign is extracted once per block in process(), valueOf never sees it).
    ppS_[PP_FB][i] = (float)(p[P5_FEEDBACK] & 127u) * (3.0f / 127.0f) * polFb_;
    // Keyed coarse tuning keeps its semitone detents (patch semantics);
    // keyboard-off B spans nine octaves continuously.
    ppS_[PP_COARSEA][i] = (float)((i_clamp((int)q[P5_FREQ_A], 0, 96) / 2 - 12) * 16);
    ppS_[PP_COARSEB][i] = (float)((i_clamp((int)q[P5_FREQ_B], 0, 96) / 2 - 12) * 16);
    ppS_[PP_ABSB][i]    = 12.0f * 16.0f + q[P5_FREQ_B] * (108.0f * 16.0f / 127.0f);
    ppS_[PP_FINEBS][i]  = q[P5_FINE_B] * (100.0f / 128.0f);
    ppS_[PP_STEPE][i]   = p[P5_GLIDE]
        ? f_max(lutU(PW_ENV_LIN, q[P5_GLIDE]) * (1.0f / 16384.0f)
                * (float)srScale_ * (1.0f / 32.0f), glideFloor_)   // E9 floor
        : 0.0f;
    const int relOn = p[P5_RELEASE_ON];
    efr_[i] = envRatesF(q, P5_ATTACK_FILTER, P5_DECAY_FILTER, P5_SUSTAIN_FILTER,
                        P5_RELEASE_FILTER, relOn, srScale_, !curtis, 0.0f);
    ear_[i] = envRatesF(q, P5_ATTACK_AMP, P5_DECAY_AMP, P5_SUSTAIN_AMP,
                        P5_RELEASE_AMP, relOn, srScale_, 0, 0.0f);
}

// what is left of p5_render per NOTE: the velocity scale and the death fade.
// Returns 1 when the lane must run through the sample kernel.
bool Engine::renderLane(Lane &l, int32_t *out, uint32_t n)
{
    VoiceState &v = l.v;
    const uint8_t *p = patch_.raw;
    // A lane whose envelope reached the kill level stops running: fade its
    // last PCM to zero over 32 host samples, continuous across call bounds.
    if (!v.active) {
        for (uint32_t i = 0; i < n && l.fadeT > 0.0f; i++)
        {
            const int32_t s = (int32_t)((float)l.lastPcm * l.fadeT);
            if (panOn_)
            {   out[i] += (int32_t)(((int64_t)s * l.panG[0]) >> 15);
                mixR_[i] += (int32_t)(((int64_t)s * l.panG[1]) >> 15); }
            else out[i] += s;
            l.fadeT -= fadeDec_;                // E9: the 32-sample fade keeps its TIME at any rate
        }
        if (l.fadeT <= 0.0f) { l.fadeT = 0.0f; l.lastPcm = 0; }
        return false;
    }
    const int32_t filterVelocity = p[P5_VEL_FILTER] ? (int32_t)v.mvel * 258 : 32767;
    const int32_t ampVelocity = p[P5_VEL_AMP] ? (int32_t)v.mvel * 258 : 32767;
    LaneParams &c = l.p;
    c.filterVel = (float)filterVelocity / 32768.0f;
    c.ampVel = (float)ampVelocity / 32768.0f * l.ampComp;   // E4 unison level comp (1.0 = exact no-op)
    return true;
}

// one block of the part's audio: the per-sample control pass, the static
// render prep, the sample kernel for the live lanes, and p5_post's coupling
// capacitor. Control NEVER advances per call: host-block and event-split
// geometry cannot move any value by even one sample's worth of slew.
void Engine::process(int32_t *out, uint32_t n)
{
    // S12: the public API used to TRUNCATE n > 32 and leave the tail
    // unwritten - a caller that skipped the plugin's chunker got silent
    // samples and a control path frozen at block 32. Loop internally instead:
    // state advances continuously across the chunks by design (per-sample).
    while (n) {
        const uint32_t k = n > (uint32_t)CTL_ ? (uint32_t)CTL_ : n;
        processBlock(out, k);
        out += k; n -= k;
    }
}

void Engine::processBlock(int32_t *out, uint32_t n)
{
    if (n > (uint32_t)CTL_) n = (uint32_t)CTL_;   // control arrays hold one block
    const uint8_t *p = patch_.raw;
    for (uint32_t i = 0; i < n; i++) { out[i] = 0; mixR_[i] = 0; }

    if (panDirty_)
    {   // slot i sits at (2i-7)/7 of the field; width scales the spread, and a
        // voice pans only AWAY from a side (linear law, gain 1 at centre-ish).
        const float t = (float)panWidth_ * (1.0f / 127.0f);
        for (int i = 0; i < MAXV; i++)
        {
            const float pos = (float)(i * 2 - (MAXV - 1)) / (float)(MAXV - 1) * t;
            int g0 = (int)(32768.0f * (pos <= 0.0f ? 1.0f : 1.0f - pos) + 0.5f);
            int g1 = (int)(32768.0f * (pos >= 0.0f ? 1.0f : 1.0f + pos) + 0.5f);
            if (g0 > 32768) g0 = 32768; if (g1 > 32768) g1 = 32768;
            if (g0 < 0) g0 = 0; if (g1 < 0) g1 = 0;
            lanes_[i].panG[0] = g0; lanes_[i].panG[1] = g1;
        }
        panOn_ = panWidth_ != 0;
        panDirty_ = false;
    }

    const float aS = smooth_ ? (float)(1.0 - std::exp(-srScale_ * (1.0 / 441.0))) : 1.0f;
    // E11: with smoothing OFF the slewed controller mirrors must EQUAL the
    // raw values - the bit-exact path then computes identical maths.
    if (!smooth_) { atS_ = (float)at_; brightS_ = (float)brightness_; mwS_ = (float)mw_; }
    const bool rngMoved = bendRngDirty_;          // E10: re-derive stale bend targets
    bendRngDirty_ = false;
    uint32_t liveChMask = 0;
    for (int k = 0; k < MAXV; k++)
        if (lanes_[k].v.active) liveChMask |= 1u << (lanes_[k].ch & 15);

    if (!smooth_)
        for (uint32_t k = 0; k < 55u; k++) fval_[k] = (float)valueOf((int)k);   // raw: exact, once
    uint32_t noiseSt = modNoise_;
    if (!noiseSt) noiseSt = 0x517953;
    // Bit-7 polarity flags (right-click on the knob). Host events drain
    // between render calls, so the patch CANNOT change inside process():
    // these are block-invariant by construction. Extract once, multiply
    // downstream — no bitwise work in the per-sample path.
    polEnv_  = 1.0f - 2.0f * (float)((p[P5_ENV_FILTER] >> 7) & 1u);
    polPoly_ = 1.0f - 2.0f * (float)((p[P5_POLY_ENV]  >> 7) & 1u);
    polPolyB_ = 1.0f - 2.0f * (float)((p[P5_POLY_B] >> 7) & 1u);
    polFb_   = 1.0f - 2.0f * (float)((p[P5_FEEDBACK]  >> 7) & 1u);
    polLfo_  = 1.0f - 2.0f * (float)((p[P5_LFO_RATE] >> 7) & 1u);
    for (uint32_t i = 0; i < n; i++)
    {
        // E11: live controllers slew on the same one-pole as the knobs, so a
        // burst of AT/wheel/CC74 messages cannot zipper (raw when unslewed).
        if (smooth_)
        {   atS_      += ((float)at_ - atS_) * aS;
            brightS_  += ((float)brightness_ - brightS_) * aS;
            mwS_      += ((float)mw_ - mwS_) * aS;
        }
        // knobs: continuous float slew; `moved` (or a flag change) gates the row
        bool moved = false;
        if (smooth_)
            for (uint32_t k = 0; k < 55u; k++)
            {
                const float tgt = (float)valueOf((int)k);
                const float d = tgt - fval_[k];
                if (d > -0.0039f && d < 0.0039f)
                {   if (fval_[k] != tgt) { fval_[k] = tgt; moved = true; }   // the snap sample refills too
                    continue; }
                fval_[k] = fval_[k] + d * aS;
                moved = true;
            }
        if (moved)
        {   partSample(i);                           // rows track the knob while it moves
            rowsSettled_ = false; }
        else if (!rowsSettled_)
        {   // params are static at this instant: one tail refill makes every
            // row valid for whatever call length this state sees next.
            for (uint32_t j = i; j < CTL_; j++) partSample(j);
            rowsSettled_ = true; }
        // bend: the old per-call quarter-step, as a per-sample factor (~2.8 ms).
        // Target re-derives only when the raw value changed; an idle, settled,
        // silent channel costs one float compare.
        for (int ch = 0; ch < 16; ch++)
        {
            // E10: a range change (RPN, MPE RCS, editor default, wire byte)
            // must re-derive the target even while the raw value sits still -
            // a bent string held across the change used to keep the OLD
            // cents-per-wheel landing.
            if (bendRaw_[ch] != bendRawSeen_[ch] || (rngMoved && bendRaw_[ch]))
            {
                bendRawSeen_[ch] = bendRaw_[ch];
                const int32_t range = bendRaw_[ch] < 0 ? rangeDnQ8(ch) : rangeUpQ8(ch);
                bendTgt_[ch] = (float)((double)bendRaw_[ch] * (double)range
                             / (bendRaw_[ch] < 0 ? 8192.0 : 8191.0));
            }
            const float d = bendTgt_[ch] - bendF_[ch];
            const bool audible = (liveChMask >> ch) & 1u;
            if (d > -0.5f && d < 0.5f)
            {   bendF_[ch] = bendTgt_[ch];
                if (!audible) continue;
            }
            else bendF_[ch] += d * (float)smoothBend_;
            const float b16 = std::floor(bendF_[ch] * (1.0f / 16.0f));
            b16H_[ch][i] = b16;
            bfiH_[ch][i] = (bendF_[ch] - b16 * 16.0f) * (2367.0f / 2560.0f);
        }
        // the shared wheel: float rate/depth, fractional phase, per sub-sample
        // (per-sample now that mwS_/atS_ slew per sample - E11)
        const float amtF = f_clamp((pcMode_ ? 0.0f : fval_[P5_LFO_INITIAL]) + mwS_
                                   + (p[P5_PRESS_LFO] ? atS_ : 0.0f), 0.0f, 127.0f);
        const double incD = (double)lutU(PW_P5_LFO_INC, fval_[P5_LFO_RATE])
                          / (double)os_ * srScale_;
        const float amount = lutF(PW_P5_AMOUNT, amtF);
        const float mix = fval_[P5_WHEEL_MIX] * (1.0f / 127.0f);
        for (uint32_t o = 0; o < (uint32_t)os_; o++)
        {
            lfoPhaseD_ += incD;
            if (lfoPhaseD_ >= 4294967296.0) lfoPhaseD_ -= 4294967296.0;
            const uint32_t phase = (uint32_t)lfoPhaseD_;
            const float noise = (float)(int32_t)(fastNoise32((int32_t *)&noiseSt) >> 8)
                              * (1.0f / 16777216.0f) - 0.5f;
            // polLfo_: right-click polarity on LFO RATE - the OUTPUT flips
            // (every destination reads w or wheelPitch_), the phase above
            // this line is untouched: no negative-increment wrap exists.
            const float w = (lfoWave(phase, p) * (1.0f - mix) + noise * mix) * amount * polLfo_;
            const uint32_t e = i * (uint32_t)os_ + o;
            wheel_[e] = w;
            wheelPitch_[e] = pitchRatio(w * (470.0f / 1200.0f));
        }
    }
    modNoise_ = (int32_t)noiseSt;

    uint32_t live = 0;
    for (int i = 0; i < MAXV; i++) live |= lanes_[i].v.active ? 1u << i : 0;

    uint32_t mask = 0;
    uint32_t nr = 0;
    bool fading = false;
    if (live) {
        const float vdep = (float)driftDepth() * (1.0f / 127.0f);
        const float leakS = driftLeak_;         // E9: activation-rate constant
        const float sigS = driftSig_;           // (was literals = 44.1k laws)
        const float kt = (float)fastClamp(p[P5_KEY_FILTER], 0, 2) * (float)PW_KEYTRACK_HALF_Q7 / 128.0f;
        const int lowB = p[P5_LOW_B], keyB = p[P5_KEY_B];
        for (int lane = 0; lane < MAXV; lane++) {
            Lane &l = lanes_[lane];
            VoiceState &v = l.v;
            if (!v.active) continue;
            const int ch = l.ch & 15;
            const float stepC = f_max((float)(PW_ENV_LIN[40] >> 14) * (float)srScale_ * (1.0f / 32.0f),
                                      glideFloor_);
            const float tgtP = (float)v.pitch16;
            const float e2 = (float)e_[2] * 16.0f, e3 = (float)e_[3] * 16.0f;
            for (uint32_t i = 0; i < n; i++)
            {
                // DRIFT: leaky (OU) walks, per sample (the ~1.5 s corner is kept;
                // at vintage 0 these stay exact zeros, so the LCG is not even run).
                float driftC = 0.0f, filtD = 0.0f, ampD = 0.0f;
                if (vdep > 0.0f)
                {
                    const uint32_t r0 = fastNoise32((int32_t *)&wSt_[lane]);
                    const uint32_t r1 = fastNoise32((int32_t *)&wSt_[lane]);
                    const uint32_t r2 = fastNoise32((int32_t *)&wSt_[lane]);
                    l.wP = l.wP * leakS + (((float)(r0 >> 8) * (1.0f / 8388608.0f)) - 1.0f) * sigS;
                    l.wF = l.wF * leakS + (((float)(r1 >> 8) * (1.0f / 8388608.0f)) - 1.0f) * sigS;
                    l.wA = l.wA * leakS + (((float)(r2 >> 8) * (1.0f / 8388608.0f)) - 1.0f) * sigS;
                    driftC = l.wP * vdep * 4.0f;     // cents, A up as B down
                    filtD  = l.wF * vdep * 110.0f;
                    ampD   = l.wA * vdep * 260.0f;
                }
                const float stepE = ppS_[PP_STEPE][i];
                if (stepE > 0.0f) l.glideF += f_clamp(tgtP - l.glideF, -stepE, stepE);
                else l.glideF = tgtP;
                l.curF += f_clamp(tgtP - l.curF, -stepC, stepC);
                // the two envelopes
                envTick1(l.fenv, efr_[i], v.gate, envCut_);
                envTick1(l.aenv, ear_[i], v.gate, envCut_);
                fenvS_[i][lane] = l.fenv.value;
                aenvS_[i][lane] = l.aenv.value;
                // VCA: ~32-sample onset ramp (E9: sample-count scales with
                // the rate so the ramp keeps its TIME), then unity (x amp drift)
                if (l.vca < 1.0f) l.vca = f_min(1.0f, l.vca + vcaStep_);
                vcaS_[i][lane] = l.vca * (1.0f + ampD * (1.0f / 32768.0f));
                // pitch stack, continuous: glide sits on the shared CV bus,
                // bend follows the lane's own channel (MPE)
                const float mPitch = f_clamp(l.curF + b16H_[ch][i], 0.0f, 2047.0f);
                const float mFine = (float)v.fine + bfiH_[ch][i];
                const float gDelta = l.glideF - l.curF;
                const float pA = applyTrim(l, mPitch + gDelta + ppS_[PP_COARSEA][i] + e2, 0);
                const float pBraw = keyB ? mPitch + gDelta + ppS_[PP_COARSEB][i] + e3
                                         : ppS_[PP_ABSB][i] + e3;
                iaS_[i][lane] = incAt(pA, driftC, mFine, srScale_, os_);
                uint32_t ib = incAt(applyTrim(l, pBraw, 1), ppS_[PP_FINEBS][i] - driftC,
                                    mFine, srScale_, os_);
                if (lowB) ib >>= 7;                 // Lo Freq drops B seven octaves.
                ibS_[i][lane] = ib;
                // cutoff adds (in float units of c.cut): drift + keyboard tracking
                // pivoting on the live pitch (the old per-block mPitch16 term).
                fcAddS_[i][lane] = (filtD + (mPitch - 576.0f) * kt) / 256.0f;
            }
            // ownenv: the engine's envelopes end the voice (p5_done)
            if (!v.gate && l.aenv.stage == 0u) {
                v.active = 0;
                v.gate = 0;
                l.fadeT = 1.0f;               // the death fade runs via renderLane
            }
        }
    }
    // E8: the render/fade pass runs over ALL lanes on EVERY call. It used to
    // sit inside the control loop behind `if (!v.active) continue`, so a
    // dying voice only faded while another voice kept the loop running, and
    // the last voice's tail stopped dead mid-fade at its block boundary.
    for (int lane = 0; lane < MAXV; lane++)
    {
        Lane &l = lanes_[lane];
        if (l.v.active) { if (renderLane(l, out, n)) { mask |= 1u << lane; nr++; } }
        else if (l.fadeT > 0.0f) { fading = true; renderLane(l, out, n); }
    }
    if (mask) {
        // Auto: below two live voices the vector kernel's fixed cost dominates,
        // so the scalar path wins; both compute bit-identical results (proven by
        // tests/test_engine_ab), switching mid-stream is safe.
        // Portable popcount (MSVC has no __builtin_popcount): 8-iteration
        // Brian-Kernighan at BLOCK rate once per process() — GCC folds it to
        // popcnt at -O3 anyway, and the integer semantics are identical.
        unsigned mm = (unsigned)mask;
        int lanes = 0;
        while (mm) { mm &= mm - 1u; lanes++; }
        const bool wide = mode_ == KernelMode::Wide ||
                          (mode_ == KernelMode::Auto && lanes >= 2);
        if (wide) simdK_(*this, mask, out, n);
        else p5KernelScalar(*this, mask, out, n);
    }

    for (uint32_t i = 0; i < n; i++) mixTap_[i] = out[i];
    if (!nr && !fading)     // E7: the DC blocker state survived only while a
    {   dcX1_ = dcY1_ = 0; dcX1r_ = dcY1r_ = 0; live_ = 0; return;
                            // kernel voice was up. On the block where the
    }                       // last voice died the cap was reset AND the fading
                            // tail skipped p5_post entirely (a DC thump with
                            // low-frequency content). Run the post on fade-
                            // only blocks too; clear the cap on a truly quiet one.
    // p5_post: the part's output coupling capacitor, about 5 Hz. Without a voice it starts afresh.
    int32_t x1 = dcX1_, y = dcY1_;
    for (uint32_t i = 0; i < n; i++) {
        const int32_t x = out[i];
        y = (int32_t)((int64_t)(x - x1) * 256 + (((int64_t)y * dcK_) >> 30));
        x1 = x;
        out[i] = (y + 128) >> 8;
    }
    dcX1_ = x1; dcY1_ = y;
    if (panOn_)
    {   // the same coupling capacitor on the right hand, with its own state
        int32_t xr = dcX1r_, yr = dcY1r_;
        for (uint32_t i = 0; i < n; i++)
        {
            const int32_t x = mixR_[i];
            yr = (int32_t)((int64_t)(x - xr) * 256 + (((int64_t)yr * dcK_) >> 30));
            xr = x;
            mixR_[i] = (yr + 128) >> 8;
        }
        dcX1r_ = xr; dcY1r_ = yr;
    }
    live_ = nr;
}

} // namespace p5
