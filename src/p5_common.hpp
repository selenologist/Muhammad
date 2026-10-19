// SPDX-License-Identifier: GPL-3.0-only
// Prophet-5 (Rev-4 wire) patch layout and SysEx frame codec, ported from
// melodee firmware/src/prophet_patch.c (GPL-3.0-only, Leo Kuroshita /
// Hügelton Instruments). Values, clamping and layouts are exact.
#pragma once
#include <cstdint>
#include <cstring>

namespace p5 {

constexpr uint32_t RAW_MAX = 134u;    // our store format: 133-byte wire patch + FEEDBACK
constexpr uint32_t WIRE_SIZE = 133u;  // Sequential's sysex/wire size - never larger
constexpr uint32_t FRAME_MAX = 159u;

enum RawIdx {
    P5_FREQ_A, P5_FREQ_B, P5_FINE_B, P5_SAW_A, P5_PULSE_A,
    P5_SAW_B, P5_TRI_B, P5_PULSE_B, P5_PW_A, P5_PW_B,
    P5_SYNC, P5_LOW_B, P5_KEY_B, P5_GLIDE, P5_LEVEL_A, P5_LEVEL_B,
    P5_NOISE, P5_CUTOFF, P5_RESONANCE, P5_KEY_FILTER, P5_FILTER_REV,
    P5_LFO_RATE, P5_LFO_INITIAL, P5_LFO_SAW, P5_LFO_TRI, P5_LFO_PULSE,
    P5_WHEEL_MIX, P5_WHEEL_FREQ_A, P5_WHEEL_FREQ_B, P5_WHEEL_PW_A,
    P5_WHEEL_PW_B, P5_WHEEL_FILTER, P5_POLY_ENV, P5_POLY_B,
    P5_POLY_FREQ, P5_POLY_PW, P5_POLY_FILTER, P5_VINTAGE,
    P5_PRESS_FILTER, P5_PRESS_LFO, P5_ENV_FILTER, P5_VEL_FILTER,
    P5_VEL_AMP, P5_ATTACK_FILTER, P5_ATTACK_AMP, P5_DECAY_FILTER,
    P5_DECAY_AMP, P5_SUSTAIN_FILTER, P5_SUSTAIN_AMP, P5_RELEASE_FILTER,
    P5_RELEASE_AMP, P5_RELEASE_ON, P5_UNISON, P5_UNISON_COUNT,
    P5_UNISON_DETUNE,
    P5_NAME = 65, P5_NAME_LEN = 20, P5_BEND = 86, P5_RETRIGGER = 87,
    // FEEDBACK: our extension byte, appended AFTER the wire payload (files
    // predate it and load with 0 = the honest default: no feedback path).
    // Per-voice VCA->filter-in regeneration, like feeding the headphone
    // output back into the Prophet's external input. Extreme values are
    // expected to distort - the softclip in front of the ladder is the
    // saturator; nothing is hidden from it.
    P5_FEEDBACK = 133
};

// Bit 7 = knob POLARITY on these bytes: right-click on the panel dials the
// flag, the face goes white, and the contribution SUBTRACTS. The polymod
// bytes flip their SOURCE sign (all routed destinations together), the env
// bytes flip the sweep, FEEDBACK flips the loop, LFO RATE flips the LFO
// OUTPUT (the phase accumulator and its rate increment never see the flag -
// a negative wrap would be undefined). The 7-bit level rides underneath
// (real wire files never set it, so imports read invert-off).
inline bool polarityByte(int rawIdx)
{   return rawIdx == P5_POLY_ENV || rawIdx == P5_POLY_B ||
           rawIdx == P5_ENV_FILTER || rawIdx == P5_FEEDBACK ||
           rawIdx == P5_LFO_RATE; }

struct Patch {
    uint8_t raw[RAW_MAX];
    uint8_t size = WIRE_SIZE, model = 0x32, command = 3, group = 0, program = 0;
};

inline void patchInit(Patch &p)
{
    std::memset(&p, 0, sizeof p);
    p.size = WIRE_SIZE; p.model = 0x32; p.command = 3;
    p.raw[P5_FREQ_A] = p.raw[P5_FREQ_B] = 24;
    p.raw[P5_VINTAGE] = 0;    // 0: stable Rev 4 .. 127: Rev 1
    p.raw[P5_SAW_A] = p.raw[P5_KEY_B] = 1;
    p.raw[P5_PW_A] = p.raw[P5_PW_B] = 64;
    p.raw[P5_LEVEL_A] = p.raw[P5_CUTOFF] = 127;
    p.raw[P5_FILTER_REV] = 1; // 0: Rev1/2 (SSI); 1: Rev3 (Curtis)
    p.raw[P5_DECAY_FILTER] = p.raw[P5_DECAY_AMP] = 60;
    p.raw[P5_SUSTAIN_FILTER] = p.raw[P5_SUSTAIN_AMP] = 127;
    p.raw[P5_RELEASE_FILTER] = p.raw[P5_RELEASE_AMP] = 35;
    p.raw[P5_RELEASE_ON] = 1;
    p.raw[P5_UNISON_COUNT] = 5;
    std::memset(p.raw + P5_NAME, ' ', P5_NAME_LEN);
    std::memcpy(p.raw + P5_NAME, "INIT", 4);
}

// Effective host-facing ranges for knobs whose raw semantics are clamped or
// defaulted by the engine (shared by the CLAP param surface and the GUI/golden
// normalizer, so the display, the behaviour and the render cannot drift).
struct KnobRange { int id; double lo, hi; bool defWhenZero; double def; };
inline const KnobRange *knobRange(int id)
{
    static constexpr KnobRange kRanges[] = {
        // Pitch law: coarse semis = clamp(raw,0,96)/2 - 12 (keyboard pitch at
        // raw 24); raws above 96 are dead on the engine, so the dial ends there.
        // OSC B FINE spans 0..127 cents (raw * 100 / 128). KEYBOARD-off B uses
        // the raw byte past 96 as a continuous freq; the dial stays honest for
        // the keyboard-tracking mode, which is what the scale numbers describe.
        { 0, 0, 96, false, 0 },   // OSC A FREQUENCY
        { 1, 0, 96, false, 0 },   // OSC B FREQUENCY
        { 2, 0, 127, false, 0 },  // OSC B FINE
        { 20, 0, 1, false, 0 },   // FILTER MODEL SSI/CURTIS (raw byte 20!)
        { 86, 0, 127, false, 0 },  // DRIFT (VINTAGE re-scoped: thermal walk depth)
        { 57, 0, 127, false, 0 },  // FEEDBACK (extension byte 133): the dial must
                                   // span its FULL travel - without this entry the
                                   // normalizer falls back to 0..255 and the knob
                                   // visibly caps at half (byte 127 = 0.498 norm)

        { 85, 0, 127, false, 0 },  // PAN WIDTH (port extension: per-voice stereo spread)
        { 19, 0, 2, false, 0 },   // KEYBD>FLT: engine fastClamps to 0/1/2 detents
        { 51, 0, 1, false, 0 },   // RELEASE ON
        { 52, 0, 1, false, 0 },   // UNISON
        { 53, 1, 8, true, 5 },    // UNISON CNT: raw 0 means "default 5"
        { 54, 0, 7, false, 0 },   // UNISON DET (raws above 7 saturate)
        { 55, 0, 11, false, 0 },  // BEND RANGE (engine clamps to 11)
        { 56, 0, 3, false, 0 },   // PRIORITY: LOW / LOW RET / LAST / LAST RET
        { 80, 0, 1, false, 0 },   // TRIM (master volume)
        { 82, -100, 100, false, 0 }, // MASTER TUNE, cents
    };
    for (const auto &r : kRanges) if (r.id == id) return &r;
    return nullptr;
}

// FEEDBACK loop filter: a 6 dB/oct one-pole at fs/4 (half of Nyquist),
// y += A*(x - y).  A = 1 - exp(-2*pi*fc/fs) = 1 - exp(-pi/2).  All-positive
// coefficients -> unit DC gain and no polarity flip, exactly what the
// feedback path needed to stop the one-sample loop ringing at the edge.
inline constexpr float kFbLpA = 0.7921204233125831f;

// 0..1 panel position for a param value (raw byte, or the param's own unit)
inline double knobNorm(int id, double value)
{
    const KnobRange *r = knobRange(id);
    // The device law is 7-bit: valueOf() clamps every continuous knob at 127,
    // so the surface ends there too — the whole sweep is audible, P5-style.
    double lo = r ? r->lo : 0.0, hi = r ? r->hi : (id < 55 ? 127.0 : 255.0);
    if (r && r->defWhenZero && value == 0) value = r->def;
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    return (value - lo) / (hi - lo);
}

// Single exact SyX frame codec (identical to the firmware; the plugin uses it
// to read Sequential's native program dumps).
inline int patchDecode(Patch &out, const uint8_t *f, uint32_t n)
{
    Patch p;
    uint32_t start, packed, size, pos = 0;
    if (!f || n < 5u || f[0] != 0xF0 || f[n-1u] != 0xF7 || f[1] != 1u ||
        (f[2] != 0x32u && f[2] != 0x31u) || (f[3] != 2u && f[3] != 3u)) return 0;
    start = f[3] == 2u ? 6u : 4u;
    if (n <= start || (f[3] == 2u && (f[4] > 9u || f[5] > 39u))) return 0;
    packed = n - start - 1u;
    size = packed == 152u ? 133u : packed == 147u ? 128u : 0u;
    if (!size) return 0;
    std::memset(&p, 0, sizeof p);
    p.size = (uint8_t)size; p.model = f[2]; p.command = f[3];
    if (p.command == 2u) { p.group = f[4]; p.program = f[5]; }
    for (uint32_t i = start; i < n - 1u;) {
        uint32_t k = size - pos < 7u ? size - pos : 7u, mask = f[i++];
        if (mask >= 128u || (mask >> k)) return 0;
        for (uint32_t j = 0; j < k; j++) {
            uint32_t b = f[i++];
            if (b >= 128u) return 0;
            p.raw[pos++] = (uint8_t)(b | (((mask >> j) & 1u) << 7));
        }
    }
    out = p;
    return 1;
}

inline uint32_t patchEncode(const Patch &p, uint8_t *f, uint32_t cap)
{
    uint32_t size = p.size, n = p.command == 2u ? 6u : 4u;
    if ((size != 128u && size != 133u) || (p.model != 0x31u && p.model != 0x32u) ||
        (p.command != 2u && p.command != 3u) ||
        (p.command == 2u && (p.group > 9u || p.program > 39u)) ||
        cap < n + size + (size + 6u) / 7u + 1u) return 0;
    f[0] = 0xF0; f[1] = 1; f[2] = p.model; f[3] = p.command;
    if (p.command == 2u) { f[4] = p.group; f[5] = p.program; }
    for (uint32_t pos = 0; pos < size;) {
        uint32_t k = size - pos < 7u ? size - pos : 7u, mask = n++;
        f[mask] = 0;
        for (uint32_t j = 0; j < k; j++, pos++) {
            f[mask] |= (uint8_t)((p.raw[pos] >> 7) << j);
            f[n++] = p.raw[pos] & 127u;
        }
    }
    f[n++] = 0xF7;
    return n;
}

// The patch a fresh instance shows: bare INIT content, curated name. The raw
// name bytes carry it, because the screen reads raw and factory programs
// already arrive in proper case from Sequential's files (no case-forcing).
inline void patchDefaultName(Patch &p)
{
    std::memset(p.raw + P5_NAME, ' ', P5_NAME_LEN);
    std::memcpy(p.raw + P5_NAME, "Peace Be Upon Him", 17);
}
inline void patchName(char *name, const Patch &p)
{
    uint32_t end = 0;
    for (uint32_t i = 0; i < P5_NAME_LEN; i++) {
        uint8_t c = p.raw[P5_NAME+i]; name[i] = (char)(c >= 32u && c <= 126u ? c : ' ');
        if (name[i] != ' ') end = i + 1u;
    }
    name[end] = 0;
}

} // namespace p5
