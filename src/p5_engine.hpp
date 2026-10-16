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
#include "p5_halfband.hpp"
#include "p5_halfband.hpp"

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

enum class KernelMode { Scalar, Wide, Auto };
class Engine;
// The two render kernels (p5_kernel_scalar.cpp / p5_kernel_simd.cpp):
// render the lanes of `mask`, n samples into out (additive), advancing the hot state.
void p5KernelScalar(Engine &e, uint32_t mask, int32_t *out, uint32_t n);
void p5KernelSimdSSE2(Engine &e, uint32_t mask, int32_t *out, uint32_t n);
#ifdef P5_HAVE_AVX2
void p5KernelSimdAVX2(Engine &e, uint32_t mask, int32_t *out, uint32_t n);
#endif

class Engine {
public:
    friend void p5KernelScalar(Engine &, uint32_t, int32_t *, uint32_t);
    friend void p5KernelSimdSSE2(Engine &, uint32_t, int32_t *, uint32_t);
#ifdef P5_HAVE_AVX2
    friend void p5KernelSimdAVX2(Engine &, uint32_t, int32_t *, uint32_t);
#endif
    explicit Engine(KernelMode mode = KernelMode::Auto);

    // ---- lifecycle / performance -------------------------------------------
    void activate(double sampleRate);         // scales every rate table by 44100/sr
    void setPatch(const Patch &p);            // native wire patch + p5_track_accept (resets the part)
    void trackAccept();                       // p5_track_accept
    void loadFactory(uint32_t k);             // Sequential's program k (0..PW_FACTORY_N-1)
    const Patch &patch() const { return patch_; }
    void patchName(char *name) const { p5::patchName(name, patch_); }
    static void patchNameAt(uint32_t factoryIndex, char *name) {   // index 0 = INIT
        if (factoryIndex == 0) { std::snprintf(name, P5_NAME_LEN + 1, "Peace Be Upon Him"); return; }
        Patch p;
        std::memset(&p, 0, sizeof p);
        std::memcpy(p.raw, PW_FACTORY_RAW[factoryIndex - 1], RAW_MAX);
        p5::patchName(name, p);
    }
    void editRaw(int idx, int v) {               // native editor write: exact byte, no state reset
        if (idx >= 0 && idx < (int)RAW_MAX) {
            patch_.raw[idx] = (uint8_t)v;
            trackAccept();
            if (idx == P5_LFO_INITIAL) wheelSeed();   // still the wheel's position, if untouched
        }
    }
    int rawValue(int idx) const { return idx >= 0 && idx < (int)RAW_MAX ? patch_.raw[idx] : 0; }
    void noteOn(int note, int vel, int ch = 0);
    void noteOff(int note, int ch = -1);   // -1: any channel (firmware semantics)
    void allNotesOff();                       // trk_all_off
    void silenceReset();                      // every voice and part state, as at boot
    void setBend(int raw) { bendRaw_[0] = i_clamp(raw, -8192, 8191); }
    // MPE: bend state is per MIDI channel; ranges resolve per channel as
    // host-set (RPN0 / MPE absolute RCS) > tuning-editor default > wire byte.
    void setBendChannel(int ch, int raw)
    { if (ch >= 0 && ch < 16) bendRaw_[ch] = i_clamp(raw, -8192, 8191); }
    int rawBendRangeUpDefault() const { return rUpDef_; }
    int rawBendRangeDownDefault() const { return rDnDef_; }
    void setBendRangeSemitones(int ch, double up, double down);       // ch<0 = all 16
    void setBendRangeDefault(double up, double down);                 // editor
    double bendRangeUpDefault() const { return rUpDef_ < 0 ? wireRangeQ8() / 256.0 : rUpDef_ / 256.0; }
    double bendRangeDownDefault() const { return rDnDef_ < 0 ? wireRangeQ8() / 256.0 : rDnDef_ / 256.0; }
    // per-MIDI-key deviation from 12-TET in 1/16-semitone units (microtuning)
    void setTuningDeviation(const int16_t *dev16);
    const int16_t *tuningDeviation() const { return tuneDev_; }
    bool tuningActive() const { return tuningActive_; }
    // PC playability mode (the plugin turns it on; the bit-exact harness keeps
    // the firmware's literal maths). LFO INITIAL is the WHEEL STATE at patch
    // load — real P5 keyboards have a reporting wheel, so the patch value is
    // the position, not an offset stacked on top of a silent zero. The first
    // real CC1 takes over and the patch value yields until the next patch.
    void setPcMode(bool on) { pcMode_ = on; if (on) wheelSeed(); }
    void wheelSeed() { if (pcMode_ && !wheelTaken_) mw_ = valueOf(P5_LFO_INITIAL); }
    void wheelReset() { wheelTaken_ = false; if (pcMode_) wheelSeed(); else mw_ = 0; }
    void setModWheel(int v) { if (pcMode_) wheelTaken_ = true; mw_ = i_clamp(v, 0, 127); }
    void setAftertouch(int v) { at_ = i_clamp(v, 0, 127); }
    void setOverlay(int i, int v) { if (i >= 0 && i < 8) e_[i] = (int16_t)v; }
    int overlay(int i) const { return e_[i]; }
    void setPolyphony(int n) { poly_ = n == 5 ? 5 : MAXV; }  // 5: the firmware's single-core cap
    // 1 = the firmware's base rate (the bit-exact path); 2 = the firmware's
    // P5_OVERSAMPLE=2 scheme (oscillators/noise/filter integrate at 2x the
    // host rate) decimated per voice by the Surge halfband instead of the
    // firmware's 2-tap box average. Not bit-exact with the firmware by
    // design; the filter cutoff table and every rate table rebuild for the
    // internal rate, so pitch, cutoff Hz and envelope times stay exact.
    void setOverSample(int os);               // 1 or 2
    // Voice kill level (linear amplitude): firmware cuts releases at 2^-10
    // (-60 dBFS). Muhammad cuts ~24 dB later for an imperceptible tail.
    void setReleaseCut(float lin);

    // GUI-side global tune in cents (-100..100): applied as fixed-point units on
    // the note's pitch16 (16 = one semitone). 0 == the firmware's exact behaviour.
    // Param slew for PC playability: block()-rate stepping of raw knobs is what
    // the device never had to worry about (its encoders move continuously).
    // Off keeps the bit-exact path; the plugin turns it on.
    void setSmoothing(bool on);
    void setTuneCents(int cents);
    int tuneCents() const { return tuneCents_; }
    int oversample() const { return os_; }
    uint32_t latencySamples() const { return os_ == 2 ? 3u : 0u; }  // halfband group delay
    int polyphony() const { return poly_; }
    int voiceMode() const { return voiceMode_; }             // 0 poly, 1 mono, 2 legato, 3 unison
    void setVoiceModeOverride(int m) { voiceModeOverride_ = m; }
    uint32_t liveVoices() const { return live_; }
    const char *simdPath() const;                   // "avx2" | "sse2" | "neon" - which wide kernel is live
    // Per-voice V/oct calibration pots (the EDIT page): scale ±63 steps of
    // ~1/16 semitone slope error across the keyboard, offset ±63 steps of
    // 1/16 semitone, both centred at 64. Applied at note-on; fixed per voice
    // until retrimmed, like the real trimmers under the lid.
    // DRIFT (the re-scoped VINTAGE): depth 0..127 of the per-voice OU walks.
    // Byte 37 stays opaque — Sequential's wire files own it, we retired the
    // firmware's reinterpretation. BRIGHTNESS (Rev4 CC74): added to cutoff.
    void setDriftDepth(int d) { driftDepth_ = d < 0 ? 0 : d > 127 ? 127 : d; }
    int  driftDepth() const { return driftDepth_; }
    void setBrightness(int b) { brightness_ = b < 0 ? 0 : b > 127 ? 127 : b; }
    void setVoiceTrim(int lane, int osc, int scale, int offset);
    void voiceTrim(int lane, int osc, int *scale, int *offset) const;
    const int32_t *lastMix() const { return mixTap_; }   // pre-post block (debug)
    void setKernelMode(KernelMode m) { mode_ = m; }

    // ---- audio ----------------------------------------------------------------
    void process(int32_t *out, uint32_t n);   // n <= 32; mono int32 mix, part post applied
    // PAN WIDTH (port extension, not a wire byte): spreads the 8 voice slots
    // across the stereo field like an Oberheim voice assigner. 0 = mono sum,
    // bit-identical to the firmware path; read R after process() via mixR().
    void setPanWidth(int w) { if (w == panWidth_) return; panWidth_ = w; panDirty_ = true; }
    int  panWidth() const { return panWidth_; }
    bool panActive() const { return panOn_; }
    const int32_t *mixR() const { return mixR_; }

private:
    struct Lane {
        VoiceState v;
        uint32_t vph[2];                      // voice_t ph[]: only the unison seeds ever advance it
        Env fenv, aenv;                       // the engine's own envelopes
        int32_t drift, filterDrift, ampDrift, envDrift;   // vintage mismatch, fixed per note
        float filterPrev, ampPrev;            // f0/a0 carried across blocks
        int32_t glidePitch;                   // s->glide_pitch (engine glide)
        int ch = 0;                           // MIDI channel of the sounding note (MPE)
        int16_t trimS[2] = {64, 64};          // per-OSC V/oct slope trim, 64 = neutral
        int16_t trimO[2] = {64, 64};          // per-OSC V/oct offset trim
        int32_t lastPcm;                      // for the one-block release fade
        int32_t panG[2] = { 32768, 32768 };   // Q15 (max 32768): Oberheim-style slot pan
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
    void tickEnvelopes(Lane &l, int gate);
    bool renderLane(Lane &l, int32_t mPitch16, int32_t mFine, int32_t mCutoff,
                    int32_t mShape, int32_t mAmp0, int32_t mAmp1, int32_t *out, uint32_t n);

    Patch patch_;
    int16_t e_[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // P_E0..P_E7 editor overlays
    uint8_t value_[55] = {0};                 // part->value: p5_value-clamped knobs
    int driftDepth_ = 0, brightness_ = 0;
    static int32_t applyTrim(const Lane &l, int32_t p16, int osc);

    float wP_[MAXV], wF_[MAXV], wA_[MAXV];    // DRIFT thermal walks: pitch, cutoff, amp
    uint32_t wSt_[MAXV];                      // walk LCGs (separate from the osc noise state)

    uint32_t lfoPhase_ = 0;
    int32_t lastPitch_ = 0, modNoise_ = 0;
    float wheel_[CTL_ * 2], wheelPitch_[CTL_ * 2];   // CTL * oversample entries
    uint32_t wheelLen_ = CTL_;
    float envCut_ = 1.0f / 1024.0f;         // ownenv release kill (tickEnvelopes)
    int32_t trackKill_ = 1 << 12;           // track ADSR kill, Q24 (adsrTick)
    int32_t dcX1_ = 0, dcY1_ = 0;
    int32_t dcX1r_ = 0, dcY1r_ = 0;           // the R-side coupling cap, same law
    int panWidth_ = 0;                        // 0..127
    bool panDirty_ = true, panOn_ = false;
    int32_t mixR_[CTL_];
    uint32_t dcK_ = DC_K_DEFAULT;
    void (*simdK_)(Engine &, uint32_t, int32_t *, uint32_t) = p5KernelSimdSSE2;
    int poly_ = MAXV;
    int tuneCents_ = 0;
    bool smooth_ = false;
    bool pcMode_ = false;
    bool wheelTaken_ = false;
    float slew_[55] = {};
    int tuneUnits_ = 0;
    int voiceMode_ = 0, voiceModeOverride_ = -1;
    int prio_ = 1, detune_ = 0;               // p5_track_accept
    int monoNote_ = 0; uint8_t monoStack_[MAXV]; uint32_t nmono_ = 0;
    uint32_t live_ = 0;

    // the track ADSR the presets leave at their defaults (preset_t: env 0,0,127,0)
    int adsrA_ = 0, adsrD_ = 0, adsrS_ = 127, adsrR_ = 0;

    int32_t bendRaw_[16] = {}, bendQ8_[16] = {};
    int16_t rUp_[16], rDn_[16];        // per-channel host range, Q8 cents, -1 = unset
    int16_t rUpDef_ = -1, rDnDef_ = -1;  // editor default, Q8, -1 = fall back to wire
    int16_t tuneDev_[128] = {};          // microtuning table, 1/16 semitone
    bool tuningActive_ = false;
    int curCh_ = 0;                      // channel of the noteOn being serviced
    int32_t wireRangeQ8() const { return (i_clamp((int32_t)patch_.raw[P5_BEND], 0, 11) + 1) * 256; }
    int16_t rangeUpQ8(int ch) const { return rUp_[ch] >= 0 ? rUp_[ch] : (rUpDef_ >= 0 ? rUpDef_ : wireRangeQ8()); }
    int16_t rangeDnQ8(int ch) const { return rDn_[ch] >= 0 ? rDn_[ch] : (rDnDef_ >= 0 ? rDnDef_ : wireRangeQ8()); }
    int mw_ = 0, at_ = 0;
    double srScale_ = 1.0;                    // 44100 / host sr
    double hostSr_ = 44100.0;
    int os_ = 1;                              // runtime P5_OVERSAMPLE
    // cutoff->g table: PW_P5_TPT_G verbatim at (44100, 1x) so the bit-exact
    // A/B never sees a recomputed value; rebuilt for the internal rate
    // (oversample * host sr) otherwise, keeping every index's cutoff Hz.
    const float *tpt_ = PW_P5_TPT_G;
    float tptGen_[PW_TPT_TOP + 1];
    void buildTpt();
    HbState hb_[MAXV];                        // per-voice 2x decimators
    uint32_t vage_ = 0;                       // voice ages: one clock for every part
    uint32_t unisonSeed_ = 0x1234567u;        // mono_play's phase-seeding LCG

    int32_t mixTap_[CTL_];
    Lane lanes_[MAXV];
    // hot state, struct-of-arrays: the SIMD kernel loads these eight lanes at a time
    uint32_t phaseA_[MAXV], phaseB_[MAXV], noise_[MAXV];
    float z_[4][MAXV], triangle_[MAXV], syncTail_[MAXV];

    KernelMode mode_ = KernelMode::Auto;
};

} // namespace p5
