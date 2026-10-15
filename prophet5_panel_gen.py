#!/usr/bin/env python3
"""Generate Prophet-5 Rev3-style front panel in lockstep:
  - prophet5_panel.svg  (flat, reusable parts; human design doc)
  - src/panel_layout.h  (machine layout; single source of truth for the NanoVG GUI)
Both outputs come from the identical emitter calls, so they cannot drift."""
import math
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
OUT_SVG = str(HERE / "prophet5_panel.svg")
OUT_HDR = str(HERE / "src" / "panel_layout.h")
PW, PH = 1560, 416
PANEL_BG = "#101010"
TXT = {"hdr": 12.0, "lbl": 8.4, "sub": 8.0, "num": 9.2, "scale": 5.6}
TRK = {"hdr": 1.6, "lbl": 0.8, "sub": 1.4, "num": 0.5, "scale": 0.0}
STY = ("hdr", "lbl", "sub", "num", "scale")

# Panel data-param -> CLAP param id (raw bytes are ids 0..56; GUI-side:
# 80 TRIM(volume) 82 MASTER TUNE 83 PROGRAM 84 BANK; -1 = decorative)
PID = {
    "osc1_freq": 0, "osc2_freq": 1, "osc2_fine": 2, "osc1_saw": 3, "osc1_sq": 4,
    "osc2_saw": 5, "osc2_tri": 6, "osc2_sq": 7, "osc1_pw": 8, "osc2_pw": 9,
    "osc1_sync": 10, "osc2_lofreq": 11, "osc2_keyboard": 12, "glide": 13,
    "mix_osc1": 14, "mix_osc2": 15, "mix_noise": 16,
    "flt_cutoff": 17, "flt_reso": 18, "flt_keyboard": 19,
    "lfo_freq": 21, "lfo_saw": 23, "lfo_tri": 24, "lfo_sq": 25,
    "wm_mix": 26, "wm_freq1": 27, "wm_freq2": 28, "wm_pw1": 29, "wm_pw2": 30, "wm_filter": 31,
    "fil_env": 32, "osc2": 33, "pm_freq1": 34, "pm_pw1": 35, "pm_filter": 36,
    "flt_envamt": 40,
    "flt_a": 43, "amp_a": 44, "flt_d": 45, "amp_d": 46,
    "flt_s": 47, "amp_s": 48, "flt_r": 49, "amp_r": 50,
    "release_btn": 51, "unison": 52, "unison_det": 54, "vintage": 37,
    "volume": 80, "master_tune": 82, "prog_bankselect": 84, "os": 81, "unison_cnt": 53,
    "lfo_preset": -1, "flt_model": 20, "prog_preset": -1, "prog_record": -1,
    "edit_btn": -1, "tune_btn": -1,
}
for _i in range(1, 9):
    PID[f"program_{_i}"] = 83   # value written = bank * 8 + (i - 1)

# Buttons whose LED follows "param nonzero" (latching switches)
SWITCHES = {"osc1_saw", "osc1_sq", "osc2_saw", "osc2_tri", "osc2_sq", "osc1_sync",
            "osc2_lofreq", "osc2_keyboard", "lfo_saw", "lfo_tri", "lfo_sq",
            "wm_freq1", "wm_freq2", "wm_pw1", "wm_pw2", "wm_filter",
            "pm_freq1", "pm_pw1", "pm_filter", "unison", "release_btn", "os",
            "flt_keyboard", "flt_model"}

def f(x):
    s = f"{x:.1f}"
    return s[:-2] if s.endswith(".0") else s

def c16(hexcol):
    return "0x" + hexcol[1:] + "u"

KNOB_R = 15.0
BTN_W, BTN_H = 24.0, 34.0

def knob_scale_parts(tlo=0, thi=10):
    n = thi - tlo
    parts = []
    for i in range(n + 1):
        a = math.radians(-140.0 + (280.0 / n) * i)
        s, c = math.sin(a), math.cos(a)
        parts.append(f'<line x1="{f(16.5*s)}" y1="{f(-16.5*c)}" x2="{f(19.5*s)}" y2="{f(-19.5*c)}" stroke-width="0.8"/>')
        parts.append(f'<text x="{f(25.2*s)}" y="{f(-25.2*c+1.8)}" class="scale">{tlo + i}</text>')
    return "".join(parts)

DEFS = f'''
    <g id="led"><circle cx="0" cy="-8" r="2.6" fill="#ff2d1a"/></g>
    <g id="ledOff"><circle cx="0" cy="-8" r="2.6" fill="#5a1512"/></g>
    <g id="btnDark">
      <rect x="-12" y="-17" width="24" height="34" rx="2.5" fill="#262626" stroke="#7a7a7a" stroke-width="1"/>
      <use xlink:href="#led" href="#led"/>
    </g>
    <g id="btnGray">
      <rect x="-12" y="-17" width="24" height="34" rx="2.5" fill="#c9c9c9" stroke="#8f8f8f" stroke-width="1"/>
      <use xlink:href="#led" href="#led"/>
    </g>
    <g id="btnOrange">
      <rect x="-12" y="-17" width="24" height="34" rx="2.5" fill="#e0781e" stroke="#a05215" stroke-width="1"/>
      <use xlink:href="#led" href="#led"/>
    </g>
    <g id="gSaw" stroke="#f0f0f0" stroke-width="1.3" fill="none"><path d="M-6,3 L6,-3 L6,3"/></g>
    <g id="gTri" stroke="#f0f0f0" stroke-width="1.3" fill="none"><path d="M-6,3 L0,-3 L6,3"/></g>
    <g id="gSq" stroke="#f0f0f0" stroke-width="1.3" fill="none"><path d="M-6,3 L-6,-3 L2,-3 L2,3"/></g>
    <g id="knobScale" fill="#e8e8e8" stroke="#e8e8e8">
      {knob_scale_parts()}
    </g>
    <g id="knobDial">
      <circle r="15" fill="#222222" stroke="#8a8a8a" stroke-width="1"/>
      <line x1="0" y1="-3" x2="0" y2="-14" stroke="#f5f5f5" stroke-width="2" stroke-linecap="round"/>
    </g>
    <g id="knobDialWhite">
      <circle r="15" fill="#e9e6df" stroke="#9a9a9a" stroke-width="1"/>
      <line x1="0" y1="-3" x2="0" y2="-14" stroke="#222222" stroke-width="2" stroke-linecap="round"/>
    </g>
'''

# ---------------- emitters (SVG + layout records) ----------------
body = []
knobs, btns, texts, boxes, rules, displays = [], [], [], [], [], []

def knob(x, y, param, label=None, angle=-60, white=False, label_dy=34, extra=None, ticks=(0, 10)):
    dial = "knobDialWhite" if white else "knobDial"
    scale = ('<use xlink:href="#knobScale" href="#knobScale"/>' if tuple(ticks) == (0, 10)
             else '<g class="scale">' + knob_scale_parts(*ticks) + '</g>')
    s = [f'<g class="knob" data-param="{param}" transform="translate({f(x)},{f(y)})">',
         scale,
         f'<g class="dial" transform="rotate({f(angle)})"><use xlink:href="#{dial}" href="#{dial}"/></g>']
    if label:
        s.append(f'<text class="lbl" y="{label_dy}">{label}</text>')
    if extra:
        s.append(extra)
    s.append('</g>')
    body.append("".join(s))
    knobs.append(dict(pid=PID[param], x=x, y=y, white=white, ticks=ticks))
    if label:
        texts.append(dict(x=x, y=y + label_dy, style="lbl", text=label))

def btn(x, y, param, color="btnDark", label=None, glyph=None, led=True,
        num_above=None, label_dy=29, label_above=None):
    s = [f'<g class="button" data-param="{param}" transform="translate({f(x)},{f(y)})">']
    s.append(f'<use xlink:href="#{color}" href="#{color}"/>')
    if not led:
        s.append('<use xlink:href="#ledOff" href="#ledOff"/>')
    if glyph:
        s.append(f'<use xlink:href="#{glyph}" href="#{glyph}" y="-24"/>')
    if num_above:
        s.append(f'<text class="num" y="-23">{num_above}</text>')
    if label_above is not None:
        s.append(f'<text class="lbl" y="{label_above}">{label}</text>')
    elif label is not None:
        s.append(f'<text class="lbl" y="{label_dy}">{label}</text>')
    s.append('</g>')
    body.append("".join(s))
    ledmode = 1 if param in SWITCHES else (2 if param.startswith("program_") else 0)
    value = int(param.split("_")[1]) - 1 if param.startswith("program_") else 0
    btns.append(dict(pid=PID[param], x=x, y=y, color=color, glyph=glyph or "",
                     led=led, ledmode=ledmode, value=value))
    if label:
        texts.append(dict(x=x, y=y + (label_above if label_above is not None else label_dy),
                          style="lbl", text=label))
    if num_above:
        texts.append(dict(x=x, y=y - 23, style="num", text=num_above))

def notes(*pairs, style="lbl"):
    """standalone labels, one body line like the hand-written original"""
    body.append("".join(f'<text class="{style}" x="{f(x)}" y="{f(y)}">{t}</text>'
                        for x, y, t in pairs))
    for x, y, t in pairs:
        texts.append(dict(x=x, y=y, style=style, text=t))

def subhead(x, y, text, tail=26):
    tw = len(text) * 5.1
    l1, l2 = x - tw / 2 - 10, x + tw / 2 + 10
    body.append(
        f'<g class="subhead"><line class="rule" x1="{f(l1-tail)}" y1="{f(y-2.6)}" x2="{f(l1)}" y2="{f(y-2.6)}"/>'
        f'<line class="rule" x1="{f(l2)}" y1="{f(y-2.6)}" x2="{f(l2+tail)}" y2="{f(y-2.6)}"/>'
        f'<text class="sub" x="{f(x)}" y="{f(y)}">{text}</text></g>')
    rules.append(dict(x1=l1 - tail, y1=y - 2.6, x2=l1, y2=y - 2.6))
    rules.append(dict(x1=l2, y1=y - 2.6, x2=l2 + tail, y2=y - 2.6))
    texts.append(dict(x=x, y=y, style="sub", text=text))

def box(x, y, w, h, header=None):
    s = [f'<rect class="box" rx="8" x="{f(x)}" y="{f(y)}" width="{f(w)}" height="{f(h)}"/>']
    if header:
        tw = len(header) * 7.4
        s.append(f'<rect class="hdrbg" x="{f(x+w/2-tw/2-4)}" y="{f(y-6)}" width="{f(tw+8)}" height="12"/>')
        s.append(f'<text class="hdr" x="{f(x+w/2)}" y="{f(y+3.6)}">{header}</text>')
    body.append("".join(s))
    boxes.append(dict(x=x, y=y, w=w, h=h, header=header or ""))

def display(x, y, w, h, inner_texts=()):
    tstr = "".join(f'<text class="lbl" x="{f(tx)}" y="{f(ty)}">{t}</text>' for tx, ty, t in inner_texts)
    body.append(f'<g class="display" data-param="display">'
                f'<rect x="{f(x)}" y="{f(y)}" width="{f(w)}" height="{f(h)}" rx="2" fill="#2b0d0d" '
                f'stroke="#7a7a7a" stroke-width="1"/>{tstr}</g>')
    displays.append(dict(x=x, y=y, w=w, h=h))
    for tx, ty, t in inner_texts:
        texts.append(dict(x=tx, y=ty, style="lbl", text=t))

def line(x1, y1, x2, y2):
    body.append(f'<line class="rule" x1="{f(x1)}" y1="{f(y1)}" x2="{f(x2)}" y2="{f(y2)}"/>')
    rules.append(dict(x1=x1, y1=y1, x2=x2, y2=y2))

# ================= layout =================
# Columns: A 8..300 | B 306..706 | MIXER 712..896 | FILTER 902..1236 | RIGHT 1242..1552
# Rows: r1 y10..130 | r2 y138..258 | r3 y266..404. FILTER box spans r1+r2.

# --- POLY-MOD (A, r1) ---
box(8, 10, 292, 120, "POLY-MOD")
knob(60, 60, "fil_env")
knob(124, 60, "osc2")
notes((60, 94, "FIL. ENV."), (124, 94, "OSC 2"))
subhead(92, 116, "SOURCE AMOUNT", 10)
for x, p, lab in ((182, "pm_freq1", "FREQ 1"), (228, "pm_pw1", "PW 1"), (276, "pm_filter", "FILTER")):
    btn(x, 60, p, label=lab, label_above=-24)
subhead(229, 106, "DESTINATION", 8)

# --- LFO (A, r2) ---
btn(30, 198, "lfo_preset", color="btnGray", label="PRESET", label_dy=34)
box(52, 138, 248, 120, "LFO")
knob(110, 196, "lfo_freq", label="FREQUENCY", label_dy=48)
for x, g, p in ((180, "gSaw", "lfo_saw"), (214, "gTri", "lfo_tri"), (248, "gSq", "lfo_sq")):
    btn(x, 196, p, glyph=g)
subhead(214, 240, "SHAPE", 10)

# --- WHEEL-MOD (A, r3) ---
box(8, 266, 292, 138, "WHEEL-MOD")
knob(62, 330, "wm_mix", angle=0,
     extra=('<text class="lbl" x="-24" y="40">LFO</text>'
            '<text class="lbl" x="26" y="40">NOISE</text>'
            '<text class="lbl" y="56">SOURCE MIX</text>'))
for x, lab, p in ((128, "FREQ 1", "wm_freq1"), (166, "FREQ 2", "wm_freq2"),
                  (204, "PW 1", "wm_pw1"), (242, "PW 2", "wm_pw2"), (280, "FILTER", "wm_filter")):
    btn(x, 330, p, label=lab, label_above=-24)
subhead(204, 380, "DESTINATION", 10)

# absolute positions of the wm_mix in-group labels (mirror the SVG offsets)
texts.append(dict(x=62 - 24, y=330 + 40, style="lbl", text="LFO"))
texts.append(dict(x=62 + 26, y=330 + 40, style="lbl", text="NOISE"))
texts.append(dict(x=62, y=330 + 56, style="lbl", text="SOURCE MIX"))

# --- OSCILLATOR 1 (B, r1) ---
box(306, 10, 400, 120, "OSCILLATOR 1")
knob(352, 60, "osc1_freq", label="FREQUENCY")
btn(436, 60, "osc1_saw", glyph="gSaw")
btn(472, 60, "osc1_sq", glyph="gSq")
subhead(454, 96, "SHAPE", 8)
knob(540, 60, "osc1_pw", label="PULSE WIDTH")
btn(620, 60, "osc1_sync", label="SYNC")

# --- OSCILLATOR 2 (B, r2) ---
box(306, 138, 400, 120, "OSCILLATOR 2")
knob(344, 196, "osc2_freq", label="FREQUENCY", label_dy=48)
knob(402, 196, "osc2_fine", label="FINE", label_dy=48)
for x, g, p in ((456, "gSaw", "osc2_saw"), (484, "gTri", "osc2_tri"), (512, "gSq", "osc2_sq")):
    btn(x, 196, p, glyph=g)
subhead(484, 240, "SHAPE", 8)
knob(562, 196, "osc2_pw", label="PULSE WIDTH", label_dy=48)
btn(614, 196, "osc2_lofreq", label="LO FREQ", label_dy=48)
btn(666, 196, "osc2_keyboard", label="KEYBOARD", label_dy=48)

# --- UNISON group (B, r3, unbordered): DETUNE / UNISON / COUNT ---
knob(372, 332, "unison_det", label="UNISON DET.", label_dy=38, ticks=(0, 7))
btn(442, 332, "unison", label="UNISON", label_dy=38)
knob(512, 332, "unison_cnt", label="COUNT", label_dy=38, ticks=(1, 8))

# --- MIXER (r1) ---
box(712, 10, 184, 120, "MIXER")
knob(756, 64, "mix_osc1", label="OSC 1")
knob(804, 64, "mix_osc2", label="OSC 2")
knob(852, 64, "mix_noise", label="NOISE")

# --- GLIDE + OVERSAMPLE (PC-side feature; the strip under MIXER) ---
knob(756, 196, "glide", label="GLIDE", label_dy=38)
btn(850, 196, "os", color="btnDark", label="OVERSAMPLE", label_dy=38)

# --- FILTER (tall r1+r2) ---
box(902, 10, 334, 248, "FILTER")
btn(928, 62, "flt_model", color="btnGray", label="MODEL")   # real-P5 filter PRESET slot -> SSI/Curtis toggle
knob(996, 58, "flt_cutoff", label="CUTOFF")
knob(1064, 58, "flt_reso", label="RESONANCE")
knob(1132, 58, "flt_envamt", label="ENVELOPE")
notes((1132, 100, "AMOUNT"))
btn(1200, 62, "flt_keyboard", label="KEYBOARD")
knob(964, 196, "flt_a", label="ATTACK", label_dy=42)
knob(1032, 196, "flt_d", label="DECAY", label_dy=42)
knob(1100, 196, "flt_s", label="SUSTAIN", label_dy=42)
knob(1168, 196, "flt_r", label="RELEASE", label_dy=42)

# --- MASTER TUNE / VOLUME (white knobs, unbordered, right) ---
knob(1330, 66, "master_tune", label="MASTER TUNE", white=True, label_dy=40)
knob(1462, 66, "volume", label="VOLUME", white=True, label_dy=40)

# --- AMPLIFIER (right, r2) ---
box(1242, 138, 310, 120, "AMPLIFIER")
knob(1296, 196, "amp_a", label="ATTACK", label_dy=44)
knob(1364, 196, "amp_d", label="DECAY", label_dy=44)
knob(1432, 196, "amp_s", label="SUSTAIN", label_dy=44)
knob(1498, 196, "amp_r", label="RELEASE", label_dy=44)

# --- PROGRAMMER (r3, shifted right; larger display; spread program row) ---
box(560, 266, 676, 138, "PROGRAMMER")
btn(598, 330, "prog_preset", color="btnGray", label="PRESET", label_dy=36)
btn(640, 330, "prog_record", color="btnOrange", label="RECORD", label_dy=36)
btn(682, 330, "prog_bankselect", color="btnGray", led=False)
notes((682, 356, "BANK"), (682, 368, "SELECT"))
display(714, 306, 150, 48, inner_texts=((762, 368, "BANK"), (836, 368, "PROGRAM")))
for i in range(8):
    btn(886 + i * 46, 330, f"program_{i+1}", color="btnGray", num_above=str(i + 1))
subhead(1047, 376, "PROGRAM SELECT", 14)

# --- RELEASE / EDIT / TUNE (r3 right, unbordered) ---
btn(1288, 330, "release_btn", color="btnDark", label="RELEASE", label_dy=36)
btn(1366, 330, "edit_btn", color="btnGray", led=False, label="EDIT", label_dy=36)
# VINTAGE replaces the hardware TUNE button (0 = stable Rev4 .. 127 = loose Rev1)
knob(1444, 330, "vintage", label="VINTAGE", label_dy=36)

# ================= assemble SVG =================
svg = f'''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     viewBox="0 0 {PW} {PH}" width="{PW}" height="{PH}">
  <style>
    text {{ font-family: Helvetica, Arial, sans-serif; fill: #f0f0f0; text-anchor: middle; }}
    .hdr   {{ font-size: {TXT["hdr"]}px; font-weight: 600; letter-spacing: {TRK["hdr"]}px; }}
    .lbl   {{ font-size: {TXT["lbl"]}px; letter-spacing: {TRK["lbl"]}px; }}
    .sub   {{ font-size: {TXT["sub"]}px; letter-spacing: {TRK["sub"]}px; }}
    .num   {{ font-size: {TXT["num"]}px; letter-spacing: {TRK["num"]}px; }}
    .scale {{ font-size: {TXT["scale"]}px; fill: #e8e8e8; letter-spacing: 0; stroke: none; }}
    .box   {{ fill: none; stroke: #e6e6e6; stroke-width: 1.2; }}
    .rule  {{ stroke: #e6e6e6; stroke-width: 1; }}
    .hdrbg {{ fill: {PANEL_BG}; stroke: none; }}
  </style>
  <defs>
{DEFS}
  </defs>

  <!-- panel background -->
  <rect width="{PW}" height="{PH}" fill="{PANEL_BG}"/>

  <!-- ============ sections ============ -->
  {chr(10).join("  " + b for b in body)}
</svg>
'''
with open(OUT_SVG, "w") as fh:
    fh.write(svg)
print("wrote", OUT_SVG, len(svg), "bytes")

# ================= emit panel_layout.h =================
def fx(v):
    return f"{float(v)}f"

def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

hdr = []
hdr.append("// GENERATED by prophet5_panel_gen.py — DO NOT EDIT.\n"
           "// The same emitter calls write prophet5_panel.svg and this file.\n"
           "#pragma once\n#include <cstdint>\n\nnamespace p5ui {\n\n")
hdr.append(f"inline constexpr float kPanelW = {PW}.0f, kPanelH = {PH}.0f;\n")
hdr.append(f"inline constexpr float kKnobR = {KNOB_R}f, kKnobLoDeg = -140.0f, kKnobHiDeg = 140.0f;\n")
hdr.append(f"inline constexpr float kBtnW = {BTN_W}f, kBtnH = {BTN_H}f;\n")
hdr.append("inline constexpr uint32_t kColPanel = " + c16(PANEL_BG) + ";\n")
for name, col in [("kColInk", "#f0f0f0"), ("kColScale", "#e8e8e8"), ("kColBox", "#e6e6e6"),
                  ("kColLedOn", "#ff2d1a"), ("kColLedOff", "#5a1512"),
                  ("kColDial", "#222222"), ("kColDialRing", "#8a8a8a"), ("kColPointer", "#f5f5f5"),
                  ("kColDialWhite", "#e9e6df"), ("kColDialWhiteRing", "#9a9a9a"), ("kColPointerDark", "#222222"),
                  ("kColBtnDark", "#262626"), ("kColBtnDarkStroke", "#7a7a7a"),
                  ("kColBtnGray", "#c9c9c9"), ("kColBtnGrayStroke", "#8f8f8f"),
                  ("kColBtnOrange", "#e0781e"), ("kColBtnOrangeStroke", "#a05215"),
                  ("kColDispBg", "#2b0d0d"), ("kColDispStroke", "#7a7a7a"),
                  ("kColDispText", "#ff5a3c")]:
    hdr.append(f"inline constexpr uint32_t {name} = {c16(col)};\n")
hdr.append("""
// text styles: size px and letter-spacing at layout scale 1
enum TxtStyle { T_HDR = 0, T_LBL, T_SUB, T_NUM, T_SCALE };
""")
hdr.append("inline constexpr float kTxtSize[5] = {" + ", ".join(f"{TXT[s]}f" for s in STY) + "};\n")
hdr.append("inline constexpr float kTxtTrack[5] = {" + ", ".join(f"{TRK[s]}f" for s in STY) + "};\n")
hdr.append("""enum BtnColor { BC_DARK = 0, BC_GRAY, BC_ORANGE };
enum Glyph { G_NONE = 0, G_SAW, G_TRI, G_SQ };
// LED modes: 0 static, 1 lit when param nonzero, 2 lit when PROGRAM == bank*8+value
struct Knob  { int pid; float x, y; bool white; int tickLo, tickHi; };
struct Btn   { int pid; float x, y; int color; int glyph; bool led; int ledmode; int value; };
struct Text  { float x, y; int style; const char *txt; };
struct Box   { float x, y, w, h; const char *header; };
struct Rule  { float x1, y1, x2, y2; };
struct Disp  { float x, y, w, h; };

""")

hdr.append(f"inline constexpr int kNumKnobs = {len(knobs)}, kNumBtns = {len(btns)}, "
           f"kNumTexts = {len(texts)}, kNumBoxes = {len(boxes)}, kNumRules = {len(rules)}, "
           f"kNumDisplays = {len(displays)};\n")

def arr(name, plural, items, fmt):
    hdr.append(f"inline constexpr {name} {plural}[] = {{\n")
    for it in items:
        hdr.append("    " + fmt(it) + "\n")
    hdr.append("};\n\n")

arr("Knob", "kKnobs", knobs, lambda k: f"    {{{k['pid']}, {fx(k['x'])}, {fx(k['y'])}, {'true' if k['white'] else 'false'}, {int(k['ticks'][0])}, {int(k['ticks'][1])}}},")
colorid = {"btnDark": "BC_DARK", "btnGray": "BC_GRAY", "btnOrange": "BC_ORANGE"}
glyphid = {"": "G_NONE", "gSaw": "G_SAW", "gTri": "G_TRI", "gSq": "G_SQ"}
arr("Btn", "kBtns", btns, lambda b: f"    {{{b['pid']}, {fx(b['x'])}, {fx(b['y'])}, {colorid[b['color']]}, "
                            f"{glyphid[b['glyph']]}, {'true' if b['led'] else 'false'}, {b['ledmode']}, {b['value']}}},")
styleid = {"hdr": "T_HDR", "lbl": "T_LBL", "sub": "T_SUB", "num": "T_NUM", "scale": "T_SCALE"}
arr("Text", "kTexts", texts, lambda t: f"    {{{fx(t['x'])}, {fx(t['y'])}, {styleid[t['style']]}, {q(t['text'])}}},")
arr("Box", "kBoxes", boxes, lambda b: f"    {{{fx(b['x'])}, {fx(b['y'])}, {fx(b['w'])}, {fx(b['h'])}, {q(b['header'])}}},")
arr("Rule", "kRules", rules, lambda r: f"    {{{fx(r['x1'])}, {fx(r['y1'])}, {fx(r['x2'])}, {fx(r['y2'])}}},")
arr("Disp", "kDisplays", displays, lambda d: f"    {{{fx(d['x'])}, {fx(d['y'])}, {fx(d['w'])}, {fx(d['h'])}}},")
hdr.append("} // namespace p5ui\n")

with open(OUT_HDR, "w") as fh:
    fh.write("".join(hdr))
print("wrote", OUT_HDR, f"({len(knobs)} knobs, {len(btns)} buttons)")
