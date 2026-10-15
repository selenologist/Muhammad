// SPDX-License-Identifier: GPL-3.0-only
// Control path of the Prophet engine: the voice-allocation and modulation work
// of melodee firmware/src/voice.c (trk_note_on/off, voice_alloc, voice_start,
// mono_play, track_render_audio) and the engine's part-level work of
// eng_prophet_test.c (p5_block, the p5_render parameter math, p5_post).
// The per-sample synthesis lives in the two kernels; this file is control-rate,
// so bit-for-bit fidelity at 44.1 kHz costs nothing per sample here.
#include "p5_engine.hpp"
#include "p5_math.hpp"

namespace p5 {

static const int V_POLY = 0, V_MONO = 1, V_LEGATO = 2, V_UNISON = 3;

Engine::Engine(bool simd) : simd_(simd)
{
    Patch p;
    patchInit(p);
    setPatch(p);
    silenceReset();
}

void Engine::activate(double sampleRate)
{
    srScale_ = sampleRate > 0.0 ? 44100.0 / sampleRate : 1.0;
    // the output coupling capacitor, about 5 Hz: k = (1 - 2 pi 5 / sr) << 30
    const double PI = 3.14159265358979323846;
    double k = (1.0 - 2.0 * PI * 5.0 / sampleRate) * 1073741824.0;
    dcK_ = (uint32_t)(k > 0.0 ? k : 0.0);
    dcX1_ = dcY1_ = 0;
    bendQ8_ = bendTarget_ = 0;
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
    trackAccept();
    monoNote_ = 0; nmono_ = 0;
    // the part's shared state starts afresh with a new patch (the p5_ready path)
    lfoPhase_ = 0; modNoise_ = 0; lastPitch_ = 0;
    for (int i = 0; i < MAXV; i++) { lanes_[i].v.active = 0; lanes_[i].v.gate = 0; }
    live_ = 0;
}

void Engine::loadFactory(uint32_t k)
{
    Patch p;
    if (k >= PW_FACTORY_N) { patchInit(p); setPatch(p); return; }
    std::memset(&p, 0, sizeof p);
    std::memcpy(p.raw, PW_FACTORY_RAW[k], RAW_MAX);
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
    for (int i = 0; i < MAXV; i++) {
        Lane &l = lanes_[i];
        std::memset(&l, 0, sizeof l);
    }
    for (int i = 0; i < MAXV; i++) {
        phaseA_[i] = phaseB_[i] = noise_[i] = 0;
        for (int j = 0; j < 4; j++) z_[j][i] = 0.0f;
        triangle_[i] = 0.0f; syncTail_[i] = 0.0f;
    }
    lfoPhase_ = 0; lastPitch_ = 0; modNoise_ = 0; dcX1_ = dcY1_ = 0;
    live_ = 0; monoNote_ = 0; nmono_ = 0;
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
        l.filterPrev = l.ampPrev = 0.0f;
        l.glidePitch = 0; l.lastPcm = 0;
        l.drift = l.filterDrift = l.ampDrift = l.envDrift = 0;
        phaseA_[lane] = l.vph[0];
        phaseB_[lane] = l.vph[1];
        for (int j = 0; j < 4; j++) z_[j][lane] = 0.0f;
        triangle_[lane] = 1.0f;
        syncTail_[lane] = 0.0f;
        // Thermal excitation lets resonance start with the oscillator mixer shut,
        // as native self-oscillating factory programs require: seed the filter.
        if (p[P5_RESONANCE] >= 110u && !p[P5_LEVEL_A] && !p[P5_LEVEL_B] && !p[P5_NOISE]) {
            z_[0][lane] = 0.5f; z_[2][lane] = -0.5f;
        }
        noise_[lane] = (uint32_t)(0x5A1793 + (int32_t)l.v.age * 17 + lane * 101);
    }
    if (!voiceWas) l.glidePitch = lastPitch_ ? lastPitch_ : l.v.pitch16;
    lastPitch_ = l.v.pitch16;
    l.fenv.stage = l.aenv.stage = 1;
    int32_t vintage = valueOf(P5_VINTAGE);
    // Fixed mismatch for the life of the voice, deterministic per note/slot.
    int32_t *st = (int32_t *)&noise_[lane];
    l.drift       = ((int32_t)(fastNoise32(st) & 255u) - 128) * vintage / 1024;
    l.filterDrift = ((int32_t)(fastNoise32(st) & 255u) - 128) * vintage / 32;
    l.ampDrift    = ((int32_t)(fastNoise32(st) & 255u) - 128) * vintage / 16;
    l.envDrift    = ((int32_t)(fastNoise32(st) & 255u) - 128) * vintage / 128;
    // The oscillators free-run, so a key finds Lo Freq B anywhere in its cycle.
    if (!voiceWas && p[P5_LOW_B]) phaseB_[lane] = fastNoise32(st);
}

void Engine::voiceStart(int lane, int note, int vel, uint8_t mvel, int *voiceWas, int *sounding)
{
    Lane &l = lanes_[lane];
    *voiceWas = !l.v.active ? 0 : (l.v.gate ? 2 : 1);
    *sounding = l.v.active && l.v.stage != 0;
    l.v.note = (uint8_t)note;
    l.v.vel = (uint8_t)vel;
    l.v.gate = 1;
    l.v.active = 1;
    l.v.stage = 1;
    l.v.age = ++vage_;
    l.v.pitch16 = note * 16;
    l.v.pitchCur = l.v.pitch16;                   // glide_set: the track's glide is off
    l.v.mvel = mvel;                              // t->m_vel (mod.c: the note's VEL)
    if (!*sounding) { l.v.env = 0; l.v.envOut = 0; }
}

void Engine::polyNoteOn(int note, int vel)
{
    // voice_alloc with P_ALLOC = 1 (REUSE: the free voice whose last pitch is closest)
    const int np = slotCount();
    int lane = -1;
    for (int i = 0; i < np; i++)
        if (lanes_[i].v.active && lanes_[i].v.note == note) { lane = i; break; }
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
    if (sounding && want == monoNote_) return;          // LOW/HIGH priority: this key does not win
    const int retrigger = mode == V_MONO ? 1 : !sounding;
    const int nv = mode == V_UNISON ? np : 1;
    for (int i = 0; i < nv; i++) {
        Lane &l = lanes_[i];
        int32_t k = 2 * i - (nv - 1);                   // -7..7
        if (nv > 1 && nv != NPOLY_) k = k * (NPOLY_ - 1) / (nv - 1);
        l.v.fine = nv > 1 ? k * detune_ * 56 / 889 : 0; // up to ~+-40 cents
        if (retrigger || !l.v.active || !l.v.gate) {
            int voiceWas, snd;
            const int v = nv >= NPOLY_ ? vel * 36 / 100 : nv > 1 ? vel / 2 : vel;
            voiceStart(i, want, v, (uint8_t)vel, &voiceWas, &snd);
            synthNoteOn(i, voiceWas);
            if (nv > 1 && i > 0) {                      // random start phases: evenly spread, they cancel
                unisonSeed_ = unisonSeed_ * 1664525u + 1013904223u;
                l.vph[0] += unisonSeed_;
            }
        } else {
            l.v.note = (uint8_t)want;                   // legato: new pitch, same envelope
            l.v.pitch16 = want * 16;
            l.v.pitchCur = l.v.pitch16;
            l.v.mvel = (uint8_t)vel;
            if (vel > l.v.vel && nv == 1) l.v.vel = (uint8_t)vel;
            if (patch_.raw[P5_RETRIGGER] & 1u) l.fenv.stage = l.aenv.stage = 1;   // p5_legato
        }
    }
    monoNote_ = want;
}

void Engine::noteOn(int note, int vel)
{
    if (note < 0 || note > 127) return;
    vel = i_clamp(vel, 1, 127);
    const int mode = voiceModeOverride_ >= 0 ? voiceModeOverride_ : voiceMode_;
    if (mode == V_POLY) polyNoteOn(note, vel);
    else monoNoteOn(note, vel);
}

void Engine::noteOff(int note)
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
                    if (lanes_[i].v.gate) { lanes_[i].v.gate = 0; lanes_[i].v.stage = 3; }
                monoNote_ = 0;
            }
        }
    }
    for (int i = 0; i < MAXV; i++)                      // a fallback already has its new pitch
        if (lanes_[i].v.note == note && lanes_[i].v.gate) {
            lanes_[i].v.gate = 0;
            lanes_[i].v.stage = 3;
        }
}

void Engine::allNotesOff()
{
    for (int i = 0; i < MAXV; i++) {
        VoiceState &v = lanes_[i].v;
        v.gate = 0;
        v.stage = v.active ? 3 : 0;
    }
    nmono_ = 0; monoNote_ = 0;
}

// ------------------------------------------------------------- part block ----
// p5_block: raw-to-DSP clamping is part-wide; envelopes and all voices share it
void Engine::block()
{
    const uint8_t *p = patch_.raw;
    for (uint32_t k = 0; k < 55u; k++) value_[k] = (uint8_t)valueOf((int)k);
    p5_update_bend();
    uint32_t inc = PW_P5_LFO_INC[value_[P5_LFO_RATE]]; // / P5_OVERSAMPLE (1)
    if (srScale_ != 1.0) inc = (uint32_t)((double)inc * srScale_);
    const int amt = i_clamp(valueOf(P5_LFO_INITIAL) + mw_ + (p[P5_PRESS_LFO] ? at_ : 0), 0, 127);
    modSamples(inc, PW_P5_AMOUNT[amt], (float)valueOf(P5_WHEEL_MIX) * (1.0f / 127.0f), wheelLen_);
}

void Engine::p5_update_bend()
{
    const int32_t range = (i_clamp((int32_t)patch_.raw[P5_BEND], 0, 11) + 1) * 256;
    bendTarget_ = bendRaw_ * range / (bendRaw_ < 0 ? 8192 : 8191);
}

// the shared wheel: LFO and noise with an exact power-of-two pitch ratio per entry
void Engine::modSamples(uint32_t inc, float amount, float mix, uint32_t entries)
{
    const uint8_t *p = patch_.raw;
    uint32_t phase = lfoPhase_;
    int32_t noiseSt = modNoise_;
    if (!noiseSt) noiseSt = 0x517953;
    for (uint32_t i = 0; i < entries; i++) {
        float noise = (float)(int32_t)(fastNoise32(&noiseSt) >> 8) * (1.0f / 16777216.0f) - 0.5f;
        float w = (lfoWave(phase, p) * (1.0f - mix) + noise * mix) * amount;
        wheel_[i] = w;
        wheelPitch_[i] = pitchRatio(w * (470.0f / 1200.0f));
        phase += inc;
    }
    modNoise_ = noiseSt;
    lfoPhase_ = phase;
}

// p5_env_tick: the engine's own envelopes (the SSM filter uses its linear decay)
float Engine::envTickEngine(Env &e, int gate, int a, int d, int s, int r, int32_t variation, int ssm)
{
    const uint8_t *p = patch_.raw;
    if (!gate && e.stage != 0u) e.stage = 3;
    float target = (float)value_[s] * (1.0f / 127.0f);
    float scale = (1.0f + (float)variation * (1.0f / 32768.0f)) * (float)srScale_;
    const uint32_t rel = p[P5_RELEASE_ON] ? value_[r] : 16u;
    if (e.stage == 1u) {
        e.value += PW_P5_ENV_ATK[value_[a]] * scale;
        if (e.value >= 1.0f) { e.value = 1.0f; e.stage = 2; }
    } else if (ssm && e.stage >= 2u) {
        const float step = PW_P5_ENV_SSM[e.stage == 2u ? value_[d] : rel] * scale;
        if (e.stage == 3u) target = 0.0f;
        e.value += f_clamp(target - e.value, -step, step);
    } else if (e.stage == 2u) {
        e.value += (target - e.value) * f_min(PW_P5_ENV_EXP[value_[d]] * scale, 1.0f);
    } else if (e.stage == 3u) {
        e.value -= e.value * f_min(PW_P5_ENV_EXP[rel] * scale, 1.0f);
    }
    if (e.stage == 3u && e.value < 1.0f / 1024.0f) { e.value = 0.0f; e.stage = 0; }
    return e.value;
}

// voice.c env_tick: the track ADSR (the Prophet presets leave it A0 D0 S127 R0)
int32_t Engine::adsrTick(Lane &l)
{
    VoiceState &v = l.v;
    const int32_t sus = adsrS_ << 17;               // Q24
    switch (v.stage) {
    case 1:
        v.env += (int32_t)PW_ENV_LIN[adsrA_ & 127];
        if (v.env >= (1 << 24)) { v.env = 1 << 24; v.stage = 2; }
        break;
    case 2:
        v.env += mulq16(sus - v.env, PW_ENV_EXP16[adsrD_ & 127]);
        break;
    case 3:
        v.env -= mulq16(v.env, PW_ENV_EXP16[adsrR_ & 127]);
        if (v.env < (1 << 12)) { v.env = 0; v.stage = 0; v.active = 0; }
        break;
    case 4:
        v.env = 0; v.stage = 0; v.active = 0;
        break;
    default:
        v.env = 0;
        break;
    }
    return v.env >> 9;
}

// p5_render: one block's parameter math for one voice, or the final fade.
// Returns 1 when the lane must run through the sample kernel.
bool Engine::renderLane(Lane &l, int32_t mPitch16, int32_t mFine, int32_t mCutoff,
                        int32_t mShape, int32_t mAmp0, int32_t mAmp1, int32_t *out, uint32_t n)
{
    VoiceState &v = l.v;
    const uint8_t *p = patch_.raw;
    const uint8_t *q = value_;
    // A budget victim is already inactive when the final fade block arrives: fade its
    // last PCM to zero instead of running a whole voice; ends exactly at zero.
    if (!v.active && !mAmp1) {
        if (n > 1u) {
            const int32_t step = l.lastPcm / (int32_t)(n - 1u);
            for (uint32_t i = 0; i < n - 1u; i++) out[i] += l.lastPcm - step * (int32_t)i;
        }
        l.lastPcm = 0;
        return false;
    }
    const float f0 = l.filterPrev, a0 = l.ampPrev;
    const int curtis = !!p[P5_FILTER_REV];
    const float f1 = envTickEngine(l.fenv, v.gate, P5_ATTACK_FILTER, P5_DECAY_FILTER,
                                   P5_SUSTAIN_FILTER, P5_RELEASE_FILTER, l.envDrift, !curtis);
    const float a1 = envTickEngine(l.aenv, v.gate, P5_ATTACK_AMP, P5_DECAY_AMP,
                                   P5_SUSTAIN_AMP, P5_RELEASE_AMP, l.envDrift, 0);
    l.filterPrev = f1;
    l.ampPrev = a1;
    int32_t glide = 0;
    if (p[P5_GLIDE]) {
        const int32_t target = v.pitch16, d = target - l.glidePitch;
        int32_t step = (int32_t)(PW_ENV_LIN[q[P5_GLIDE]] >> 14);
        if (srScale_ != 1.0) step = (int32_t)((double)step * srScale_);
        if (step < 1) step = 1;
        l.glidePitch += i_clamp(d, -step, step);
        glide = l.glidePitch - v.pitchCur;
    } else {
        l.glidePitch = v.pitch16;
    }
    // Keyed coarse tuning has semitone detents (adjacent raw values share one);
    // keyboard-off B spans nine octaves continuously. Only the rendered pitch is
    // quantized; the patch bytes stay exact.
    const int32_t coarse_a = (i_clamp(q[P5_FREQ_A], 0, 96) / 2 - 12) * 16;
    const int32_t coarse_b = (i_clamp(q[P5_FREQ_B], 0, 96) / 2 - 12) * 16;
    const int32_t pa = mPitch16 + glide + coarse_a + (int32_t)e_[2] * 16;
    const int32_t pb = (p[P5_KEY_B] ? mPitch16 + glide + coarse_b
                                    : 12 * 16 + (int32_t)q[P5_FREQ_B] * 108 * 16 / 127) + (int32_t)e_[3] * 16;
    uint32_t ia = centsInc(pa, l.drift, mFine);
    uint32_t ib = centsInc(pb, (int32_t)q[P5_FINE_B] * 100 / 128 - l.drift, mFine);
    if (srScale_ != 1.0) {                            // pitch is exact at any host sr
        ia = (uint32_t)((double)ia * srScale_);
        ib = (uint32_t)((double)ib * srScale_);
    }
    // Lo Freq drops B seven octaves.
    if (p[P5_LOW_B]) ib >>= 7;
    const int32_t filterVelocity = p[P5_VEL_FILTER] ? (int32_t)v.mvel * 258 : 32767;
    const int32_t ampVelocity = p[P5_VEL_AMP] ? (int32_t)v.mvel * 258 : 32767;
    // Cutoff in Q8 steps of the coefficient table (14.02 per octave, 30 Hz at 0).
    const int32_t c0 = q[P5_CUTOFF];
    int32_t cut = 1618 + c0 * 236 + (curtis ? 0 : (127 - c0 > 66 ? 66 : 127 - c0) * 38)
                + mCutoff + (int32_t)e_[0] * 256 + l.filterDrift;
    // Keyboard tracking pivots on the lowest C: full tracks 2x per octave, half sqrt(2).
    cut += (mPitch16 - 36 * 16) * fastClamp(p[P5_KEY_FILTER], 0, 2) * PW_KEYTRACK_HALF_Q7 / 128;
    if (p[P5_PRESS_FILTER]) cut += at_ * 96;
    const int32_t resonance = fastClamp((int32_t)q[P5_RESONANCE] + e_[1], 0, 127);
    const int32_t k = resonance * (curtis ? 19200 : 18800) / 127;
    const int32_t filterGain = 32767 - resonance * (curtis ? 100 : 45);
    const int32_t envAmount = q[P5_ENV_FILTER] * 165;     // 0.046 octave per step (Rev-4 recordings)
    // Poly-Mod: oscillator A in Q12 octaves, filter in Q8 steps.
    const int32_t paEnv = q[P5_POLY_ENV] * 85, paB = q[P5_POLY_B] * 35;
    const int32_t pfEnv = q[P5_POLY_ENV] * 226, pfB = q[P5_POLY_B] * 226;
    const int32_t pwEnv = q[P5_POLY_ENV] * 64, pwB = q[P5_POLY_B] * 258;
    const int32_t widthMod = mShape - (64 << 8);
    const int32_t pwmA = (int32_t)q[P5_PW_A] * 258 + (int32_t)e_[4] * 256 + widthMod;
    const int32_t pwmB = (int32_t)q[P5_PW_B] * 258 + (int32_t)e_[5] * 256 + widthMod;
    const int32_t la = i_clamp((int32_t)q[P5_LEVEL_A] + e_[6], 0, 127) * 258;
    const int32_t lb = i_clamp((int32_t)q[P5_LEVEL_B] + e_[7], 0, 127) * 258;
    const int32_t ln = (int32_t)q[P5_NOISE] * 258;

    LaneParams &c = l.p;
    c.f0 = f0; c.a0 = a0; c.f1 = f1; c.a1 = a1;
    c.cut = (float)cut / 256.0f;
    c.k = (float)k / 4096.0f;
    c.filterGain = (float)filterGain / 32768.0f;
    c.filterVel = (float)filterVelocity / 32768.0f;
    c.ampVel = (float)ampVelocity / 32768.0f;
    c.envAmount = (float)envAmount / 256.0f;
    c.paEnv = (float)paEnv / 4096.0f;
    c.paB = (float)paB / 4096.0f;
    c.pfEnv = (float)pfEnv / 256.0f;
    c.pfB = (float)pfB / 256.0f;
    c.pwEnv = (float)pwEnv / 32768.0f;
    c.pwB = (float)pwB / 32768.0f;
    c.pwmA = (float)pwmA / 32768.0f;
    c.pwmB = (float)pwmB / 32768.0f;
    c.la = (float)la / 32768.0f;
    c.lb = (float)lb / 32768.0f;
    c.ln = (float)ln / 32768.0f;
    c.ia = ia;
    c.ib = ib;
    c.amp0 = (float)mAmp0;
    c.amp1 = (float)mAmp1;
    return true;
}

// one block of the part's audio: e->block, the per-voice vmod and render prep,
// the sample kernel for the live lanes, and p5_post's coupling capacitor.
void Engine::process(int32_t *out, uint32_t n)
{
    wheelLen_ = n <= CTL_ ? n : CTL_;             // callers chop host blocks to <= CTL
    for (uint32_t i = 0; i < n; i++) out[i] = 0;

    uint32_t live = 0;
    for (int i = 0; i < MAXV; i++) live |= lanes_[i].v.active ? 1u << i : 0;

    block();                                      // the part's wheel runs even on a silent track

    {   // bend smoothing (~6 ms, exact landing), then this block's bend split
        int32_t d = bendTarget_ - bendQ8_;
        bendQ8_ += d > 0 ? (d < 4 ? d : (d + 3) / 4) : (d > -4 ? d : (d - 3) / 4);
    }
    const int32_t bend = bendQ8_;
    const int32_t bend16 = bend >= 0 ? bend / 16 : -((-bend + 15) / 16);
    const int32_t bendFine = (bend - bend16 * 16) * 2367 / 2560;

    uint32_t mask = 0;
    uint32_t nr = 0;
    if (live) {
        for (int i = 0; i < MAXV; i++) {
            Lane &l = lanes_[i];
            VoiceState &v = l.v;
            if (!v.active) continue;
            int32_t env = adsrTick(l);
            if (v.active) {                       // ownenv: the engine's envelopes end the voice
                if (!v.gate && l.aenv.stage == 0u) {      // p5_done: its last block, faded out
                    v.active = v.gate = 0;
                    v.stage = 0;
                    v.env = 0;
                    env = 0;
                } else {
                    v.env = 1 << 24;                      // (the common ADSR holds at full)
                    env = 32767;
                }
            }
            const int32_t envq15 = (int32_t)(l.ampPrev * 32768.0f);   // p5_env_source
            const int32_t amp1 = env;                   // ownenv: no common velocity gain here
            const int32_t amp0 = v.envOut;
            v.envOut = amp1;
            if (v.pitchCur != v.pitch16) {              // the track's glide (off: pitchCur == pitch16)
                int32_t d = v.pitch16 - v.pitchCur;
                int32_t st = (int32_t)(PW_ENV_LIN[40] >> 14);
                if (st < 1) st = 1;
                v.pitchCur += d > 0 ? (d < st ? d : st) : (-d < st ? d : -st);
            }
            if (!env && !amp0 && v.stage == 2) continue;    // held at a silent sustain
            const int32_t mPitch16 = i_clamp(v.pitchCur + bend16, 0, 2047);
            const int32_t mFine = v.fine + bendFine;
            const int32_t mCutoff = v.vel > 110 ? (envq15 * 24) >> 7 : 0;    // accent opens the filter
            if (renderLane(l, mPitch16, mFine, mCutoff, 64 << 8, amp0, amp1, out, n)) {
                mask |= 1u << i;
                nr++;
            }
        }
    }
    if (mask) {
        if (simd_) p5KernelSimd(*this, mask, out, n);
        else p5KernelScalar(*this, mask, out, n);
    }

    if (!nr) { dcX1_ = dcY1_ = 0; live_ = 0; return; }
    // p5_post: the part's output coupling capacitor, about 5 Hz. Without a voice it starts afresh.
    int32_t x1 = dcX1_, y = dcY1_;
    for (uint32_t i = 0; i < n; i++) {
        const int32_t x = out[i];
        y = (int32_t)((int64_t)(x - x1) * 256 + (((int64_t)y * dcK_) >> 30));
        x1 = x;
        out[i] = (y + 128) >> 8;
    }
    dcX1_ = x1; dcY1_ = y;
    live_ = nr;
}

} // namespace p5
