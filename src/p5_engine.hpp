// SPDX-License-Identifier: GPL-3.0-only
// Prophet-5 engine ported from melodee firmware/src/eng_prophet_test.c and the
// voice/render path of firmware/src/voice.c. One part, MAXV voice slots; the
// control path is shared by two render kernels:
//   p5KernelScalar - a literal transcription (bit-exact reference)
//   p5KernelSimd   - the same maths with the eight voices 8 lanes wide (SIMDe)
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "p5_common.hpp"
#include "p5_tables.h"
#include "p5_math.hpp"

namespace p5 {

constexpr int MAXV = 8;                 // the dual-core engine's eight voices: one SIMD group
constexpr int CTL_ = PW_CTL;            // one control tick = 32 samples
constexpr int VOICE_FS = 24000;         // dsp.c: one voice peaks near -6 dBFS
constexpr uint32_t DC_K_DEFAULT = 1072976913u;  // (1 - 2 pi 5 / 44100) << 30, eng_prophet_test.c
constexpr int NPOLY_ = 8;               // core.h NPOLY (the unison spread width reference)

struct Env { float value; int32_t stage; };   // p5_env_t: 0 off, 1 attack, 2 decay/sustain, 3 release

// Voice bookkeeping from voice.c's voice_t (the fields this path touches)
struct VoiceState {
    uint8_t note, vel, gate, active, stage;   // stage: the track ADSR's
    int32_t env, envOut;                      // Q24 / Q15
    int32_t pitch16, pitchCur, fine;
    uint32_t age;
    uint8_t mvel;
};

// per-voice block parameters: p5_render_params_t of eng_prophet_test.c
struct LaneParams {
    float f0, a0, f1, a1, cut, k, filterGain, filterVel, ampVel, envAmount;
    float paEnv, paB, pfEnv, pfB, pwEnv, pwB, pwmA, pwmB, la, lb, ln;
    float amp0, amp1;                         // m.amp0/m.amp1, Q15 as float
    uint32_t ia, ib;
};

class Engine;
// The two render kernels (p5_kernel_scalar.cpp / p5_kernel_simd.cpp):
// render the lanes of `mask`, n samples into out (additive), advancing the hot state.
void p5KernelScalar(Engine &e, uint32_t mask, int32_t *out, uint32_t n);
void p5KernelSimd(Engine &e, uint32_t mask, int32_t *out, uint32_t n);

class Engine {
public:
    friend void p5KernelScalar(Engine &, uint32_t, int32_t *, uint32_t);
    friend void p5KernelSimd(Engine &, uint32_t, int32_t *, uint32_t);
    explicit Engine(bool simd = true);

    // ---- lifecycle / performance -------------------------------------------
    void activate(double sampleRate);         // scales every rate table by 44100/sr
    void setPatch(const Patch &p);            // native wire patch + p5_track_accept (resets the part)
    void trackAccept();                       // p5_track_accept
    void loadFactory(uint32_t k);             // Sequential's program k (0..PW_FACTORY_N-1)
    const Patch &patch() const { return patch_; }
    void patchName(char *name) const { p5::patchName(name, patch_); }
    static void patchNameAt(uint32_t factoryIndex, char *name) {   // index 0 = INIT
        if (factoryIndex == 0) { std::snprintf(name, P5_NAME_LEN + 1, "INIT PROPHET"); return; }
        Patch p;
        std::memset(&p, 0, sizeof p);
        std::memcpy(p.raw, PW_FACTORY_RAW[factoryIndex - 1], RAW_MAX);
        p5::patchName(name, p);
    }
    void editRaw(int idx, int v) {               // native editor write: exact byte, no state reset
        if (idx >= 0 && idx < (int)RAW_MAX) { patch_.raw[idx] = (uint8_t)v; trackAccept(); }
    }
    int rawValue(int idx) const { return idx >= 0 && idx < (int)RAW_MAX ? patch_.raw[idx] : 0; }
    void noteOn(int note, int vel);
    void noteOff(int note);
    void allNotesOff();                       // trk_all_off
    void silenceReset();                      // every voice and part state, as at boot
    void setBend(int raw) { bendRaw_ = i_clamp(raw, -8192, 8191); }
    void setModWheel(int v) { mw_ = i_clamp(v, 0, 127); }
    void setAftertouch(int v) { at_ = i_clamp(v, 0, 127); }
    void setOverlay(int i, int v) { if (i >= 0 && i < 8) e_[i] = (int16_t)v; }
    int overlay(int i) const { return e_[i]; }
    void setPolyphony(int n) { poly_ = n == 5 ? 5 : MAXV; }  // 5: the firmware's single-core cap
    int polyphony() const { return poly_; }
    int voiceMode() const { return voiceMode_; }             // 0 poly, 1 mono, 2 legato, 3 unison
    void setVoiceModeOverride(int m) { voiceModeOverride_ = m; }
    uint32_t liveVoices() const { return live_; }
    void setUseSimd(bool s) { simd_ = s; }

    // ---- audio ----------------------------------------------------------------
    void process(int32_t *out, uint32_t n);   // n <= 32; mono int32 mix, part post applied

private:
    struct Lane {
        VoiceState v;
        uint32_t vph[2];                      // voice_t ph[]: only the unison seeds ever advance it
        Env fenv, aenv;                       // the engine's own envelopes
        int32_t drift, filterDrift, ampDrift, envDrift;   // vintage mismatch, fixed per note
        float filterPrev, ampPrev;            // f0/a0 carried across blocks
        int32_t glidePitch;                   // s->glide_pitch (engine glide)
        int32_t lastPcm;                      // for the one-block release fade
        LaneParams p;
    };

    int valueOf(int k) const { return patch_.raw[k] > 127u ? 127 : patch_.raw[k]; }
    int slotCount() const;                    // p5_cap / trk_nvoice
    void p5_update_bend();
    void block();                             // p5_block (part->value, bend, wheel)
    void modSamples(uint32_t inc, float amount, float mix, uint32_t entries);
    float envTickEngine(Env &e, int gate, int a, int d, int s, int r, int32_t variation, int ssm);
    int32_t adsrTick(Lane &l);                // voice.c env_tick (track ADSR; preset defaults)
    void synthNoteOn(int lane, int voiceWas); // p5_note_on
    void voiceStart(int lane, int note, int vel, uint8_t mvel, int *voiceWas, int *sounding);
    void polyNoteOn(int note, int vel);
    void monoNoteOn(int note, int vel);
    int monoPick() const;
    bool renderLane(Lane &l, int32_t mPitch16, int32_t mFine, int32_t mCutoff,
                    int32_t mShape, int32_t mAmp0, int32_t mAmp1, int32_t *out, uint32_t n);

    Patch patch_;
    int16_t e_[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // P_E0..P_E7 editor overlays
    uint8_t value_[55] = {0};                 // part->value: p5_value-clamped knobs
    uint32_t lfoPhase_ = 0;
    int32_t lastPitch_ = 0, modNoise_ = 0;
    float wheel_[CTL_], wheelPitch_[CTL_];
    uint32_t wheelLen_ = CTL_;
    int32_t dcX1_ = 0, dcY1_ = 0;
    uint32_t dcK_ = DC_K_DEFAULT;
    int poly_ = MAXV;
    int voiceMode_ = 0, voiceModeOverride_ = -1;
    int prio_ = 1, detune_ = 0;               // p5_track_accept
    int monoNote_ = 0; uint8_t monoStack_[MAXV]; uint32_t nmono_ = 0;
    uint32_t live_ = 0;

    // the track ADSR the presets leave at their defaults (preset_t: env 0,0,127,0)
    int adsrA_ = 0, adsrD_ = 0, adsrS_ = 127, adsrR_ = 0;

    int32_t bendRaw_ = 0, bendQ8_ = 0, bendTarget_ = 0;
    int mw_ = 0, at_ = 0;
    double srScale_ = 1.0;                    // 44100 / host sr
    uint32_t vage_ = 0;                       // voice ages: one clock for every part
    uint32_t unisonSeed_ = 0x1234567u;        // mono_play's phase-seeding LCG

    Lane lanes_[MAXV];
    // hot state, struct-of-arrays: the SIMD kernel loads these eight lanes at a time
    uint32_t phaseA_[MAXV], phaseB_[MAXV], noise_[MAXV];
    float z_[4][MAXV], triangle_[MAXV], syncTail_[MAXV];

    bool simd_ = true;
};

} // namespace p5
