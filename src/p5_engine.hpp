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
    uint8_t note, vel, gate, active;
    int32_t pitch16, pitchCur, fine;          // pitchCur: the allocation's last pitch
    uint32_t age;
    uint8_t mvel;
};

// The part-wide parameter math is a function of the per-sample knob state, so
// it lives in a per-sample broadcast row (ppS_) next to the envelopes/pitch.
enum PartP { PP_CUT, PP_K, PP_FG, PP_ENVAMT, PP_PAENV, PP_PAB, PP_PFENV, PP_PFB,
             PP_PWENV, PP_PWB, PP_PWMA, PP_PWMB, PP_LA, PP_LB, PP_LN, PP_FB,
             PP_COARSEA, PP_COARSEB, PP_ABSB, PP_FINEBS, PP_STEPE, PP_N };
// per-sample envelope rate sets (built in partSample from the live knob state)
struct EnvRates
{
    float atk, tgt, expD, expR, ssmD, ssmR;
    int ssm;
};
// per-voice parameters that are genuinely per-NOTE (velocity scale): constant
// between note events, so they stay per-call. Everything else is per-sample.
struct LaneParams {
    float filterVel, ampVel;
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
        std::memcpy(p.raw, PW_FACTORY_RAW[factoryIndex - 1], WIRE_SIZE);
        p.raw[P5_FEEDBACK] = 0;   // factory wire data predates our extension
        p5::patchName(name, p);
    }
    void editRaw(int idx, int v) {               // native editor write: exact byte, no state reset
        if (idx >= 0 && idx < (int)RAW_MAX) {
            patch_.raw[idx] = (uint8_t)v;
            paramDirty_ = true;
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
    void setModWheel(int v) { if (pcMode_) wheelTaken_ = true; mw_ = i_clamp(v, 0, 127);
                              if (mw_ != wheelSeen_) { wheelSeen_ = mw_; } }
    // A saved patch must sound the way it sounds. Once the real wheel has
    // taken over (pc mode), the wire INITIAL byte - the ONLY thing that
    // persists - is stale (typically the 0 it loads at), so save-without-
    // this wrote a silent wheel: depth "zeroed until you touch it again".
    // Sync the heard position into the byte before any serialization.
    void commitWheelToPatch()
    {   if (pcMode_ && wheelTaken_) editRaw(P5_LFO_INITIAL, mw_); }
    int wheelPos() const { return mw_; }
    void setAftertouch(int v) { at_ = i_clamp(v, 0, 127); paramDirty_ = true; }
    void setOverlay(int i, int v) { if (i >= 0 && i < 8) { e_[i] = (int16_t)v; paramDirty_ = true; } }
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
    // It rides wire byte 37 (P5_VINTAGE) — the firmware's own 0..127 "looseness"
    // slot: our use covers basically the same range, so saved patches carry the
    // knob like every other control and no side-channel trailer is needed.
    // Factory wire files carry real values here (0..127 across the bank,
    // mode 42) - "loose" presets drift at their recorded depth; that is
    // the byte's own looseness axis, used faithfully. Bit-exact harness
    // unaffected: ref dumps zero byte 37 already.
    // BRIGHTNESS (Rev4 CC74): added to cutoff.
    void setDriftDepth(int d) { editRaw(P5_VINTAGE, d < 0 ? 0 : d > 127 ? 127 : d); }
    int  driftDepth() const { return valueOf(P5_VINTAGE); }
    void setBrightness(int b) { brightness_ = b < 0 ? 0 : b > 127 ? 127 : b; paramDirty_ = true; }
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
        Env fenv, aenv;                       // the engine's own envelopes (ticked per sample)
        int32_t envDrift;                     // (kept: the note-on roulette slot, always 0)
        float glideF, curF;                   // engine glide + track glide, 1/16-st, continuous
        float vca, fadeT;                     // VCA onset ramp / death fade, per sample
        float wP, wF, wA;                     // per-lane drift walks (was wP_[i] etc)
        int ch = 0;                           // MIDI channel of the sounding note (MPE)
        int16_t trimS[2] = {64, 64};          // per-OSC V/oct slope trim, 64 = neutral
        int16_t trimO[2] = {64, 64};          // per-OSC V/oct offset trim
        int32_t lastPcm;                      // for the one-block release fade
        int32_t panG[2] = { 32768, 32768 };   // Q15 (max 32768): Oberheim-style slot pan
        LaneParams p;
    };

    int valueOf(int k) const
    {   const uint8_t r = patch_.raw[k];
        // Bit 7 = POLARITY flag on these (right-click on the knob);
        // valueOf must never see the flag, so the level continues underneath.
        if (polarityByte(k)) return (int)(r & 127u);
        return r > 127u ? 127 : (int)r; }
    int slotCount() const;                    // p5_cap / trk_nvoice
    // (the old p5_block/adsrTick/tickEnvelopes control ticks are gone: the
    // whole control path advances per host sample inside process())
    void synthNoteOn(int lane, int voiceWas); // p5_note_on
    void voiceStart(int lane, int note, int vel, uint8_t mvel, int *voiceWas, int *sounding);
    void polyNoteOn(int note, int vel);
    void monoNoteOn(int note, int vel);
    int monoPick() const;
    bool renderLane(Lane &l, int32_t *out, uint32_t n);   // velocity scale + death fade
    void partSample(uint32_t i);                 // ppS_ column i: knob state -> DSP params

    Patch patch_;
    int16_t e_[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // P_E0..P_E7 editor overlays
    float fval_[55] = {0};                    // part->value, continuous (float slew, no byte stair)
    int brightness_ = 0;
    static float applyTrim(const Lane &l, float p16, int osc);   // float now: pitch is continuous

    uint32_t wSt_[MAXV];                      // walk LCGs (separate from the osc noise stream)

    double lfoPhaseD_ = 0;                  // fractional LFO phase (per-sub-sample float rate)
    int32_t lastPitch_ = 0, modNoise_ = 0;
    float wheel_[CTL_ * 2], wheelPitch_[CTL_ * 2];   // CTL * oversample entries
    uint32_t wheelLen_ = CTL_;
    float envCut_ = 1.0f / 1024.0f;         // release kill (env tick)
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
    bool paramDirty_ = true;                  // partSample() must rebuild the ppS_ rows
    // Block-invariant polarity signs (bit 7 of the flag bytes, see process()).
    float polEnv_ = 1.0f, polPoly_ = 1.0f, polPolyB_ = 1.0f, polFb_ = 1.0f, polLfo_ = 1.0f;
    float bendTgt_[16] = {};
    int32_t bendRawSeen_[16] = {};
    int wheelSeen_ = -1;
    bool pcMode_ = false;
    bool wheelTaken_ = false;
    int tuneUnits_ = 0;
    int voiceMode_ = 0, voiceModeOverride_ = -1;
    int prio_ = 1, detune_ = 0;               // p5_track_accept
    int monoNote_ = 0; uint8_t monoStack_[MAXV]; uint32_t nmono_ = 0;
    uint32_t live_ = 0;

    int32_t bendRaw_[16] = {};
    float bendF_[16] = {};                  // continuous bend, Q8 cents, per-sample slew
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
    double smoothBend_ = 0.008990;            // per-sample bend slew factor (set in activate)
    double hostSr_ = 44100.0;
    int os_ = 1;                              // runtime P5_OVERSAMPLE
    // cutoff->g table: PW_P5_TPT_G verbatim at (44100, 1x) so the bit-exact
    // A/B never sees a recomputed value; rebuilt for the internal rate
    // (oversample * host sr) otherwise, keeping every index's cutoff Hz.
    const float *tpt_ = PW_P5_TPT_G;
    float tptGen_[PW_TPT_TOP + 1];
    void buildTpt();
    HbState hbMixL_, hbMixR_;               // THE decimator: once, after the
                                            // whole mix (R lane only if pan)
    uint32_t vage_ = 0;                       // voice ages: one clock for every part
    uint32_t unisonSeed_ = 0x1234567u;        // mono_play's phase-seeding LCG

    int32_t mixTap_[CTL_];
    Lane lanes_[MAXV];
    // per-sample control, materialized in process() and read by BOTH kernels
    // (SoA columns of 8 lanes): the engine has no control-rate grid anymore.
    float fenvS_[CTL_][MAXV], aenvS_[CTL_][MAXV];   // the two envelopes, this call's samples
    float vcaS_[CTL_][MAXV];                        // VCA gain: onset ramp / 1 / (1+ampDrift)
    float fcAddS_[CTL_][MAXV];                      // cutoff adds: filter drift + key-track, /256
    uint32_t iaS_[CTL_][MAXV], ibS_[CTL_][MAXV];    // per-sample oscillator increments
    float b16H_[16][CTL_], bfiH_[16][CTL_];         // per-channel bend history this block
    float ppS_[PP_N][CTL_];                         // part parameters, per host sample
    EnvRates efr_[CTL_], ear_[CTL_];                // the two env laws, per host sample
    // hot state, struct-of-arrays: the SIMD kernel loads these eight lanes at a time
    uint32_t phaseA_[MAXV], phaseB_[MAXV], noise_[MAXV];
    float fbState_[MAXV] = {};       // FEEDBACK: last sample's VCA out, per voice
    float z_[4][MAXV], triangle_[MAXV], syncTail_[MAXV];

    KernelMode mode_ = KernelMode::Auto;
};

} // namespace p5
