#!/usr/bin/env python3
"""Generate Prophet-5 Rev3-style front panel SVG (flat, reusable parts)."""
import math, os
import pathlib

OUT = str(pathlib.Path(__file__).resolve().parent / "prophet5_panel.svg")
PW, PH = 1560, 416
PANEL_BG = "#161616"

def f(x):
    s = f"{x:.1f}"
    return s[:-2] if s.endswith(".0") else s

# ---------------- defs ----------------
def knob_scale_parts():
    parts = []
    for v in range(11):
        a = math.radians(-140 + 28 * v)
        s, c = math.sin(a), math.cos(a)
        parts.append(f'<line x1="{f(16.5*s)}" y1="{f(-16.5*c)}" x2="{f(19.5*s)}" y2="{f(-19.5*c)}" stroke-width="0.8"/>')
        parts.append(f'<text x="{f(25.2*s)}" y="{f(-25.2*c+1.8)}" class="scale">{v}</text>')
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

# ---------------- emitters ----------------
body = []

def knob(x, y, param, label=None, angle=-60, white=False, label_dy=34, extra=None):
    dial = "knobDialWhite" if white else "knobDial"
    s = [f'<g class="knob" data-param="{param}" transform="translate({f(x)},{f(y)})">',
         f'<use xlink:href="#knobScale" href="#knobScale"/>',
         f'<g class="dial" transform="rotate({f(angle)})"><use xlink:href="#{dial}" href="#{dial}"/></g>']
    if label:
        s.append(f'<text class="lbl" y="{label_dy}">{label}</text>')
    if extra:
        s.append(extra)
    s.append('</g>')
    body.append("".join(s))

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

def subhead(x, y, text, tail=26):
    tw = len(text) * 4.6
    l1, l2 = x - tw / 2 - 10, x + tw / 2 + 10
    body.append(
        f'<g class="subhead"><line class="rule" x1="{f(l1-tail)}" y1="{f(y-2.6)}" x2="{f(l1)}" y2="{f(y-2.6)}"/>'
        f'<line class="rule" x1="{f(l2)}" y1="{f(y-2.6)}" x2="{f(l2+tail)}" y2="{f(y-2.6)}"/>'
        f'<text class="sub" x="{f(x)}" y="{f(y)}">{text}</text></g>')

def box(x, y, w, h, header=None):
    s = [f'<rect class="box" rx="8" x="{f(x)}" y="{f(y)}" width="{f(w)}" height="{f(h)}"/>']
    if header:
        tw = len(header) * 6.8
        s.append(f'<rect class="hdrbg" x="{f(x+w/2-tw/2-4)}" y="{f(y-6)}" width="{f(tw+8)}" height="12"/>')
        s.append(f'<text class="hdr" x="{f(x+w/2)}" y="{f(y+3.6)}">{header}</text>')
    body.append("".join(s))

# ================= layout =================
# Columns: A 8..300 | B 306..706 | MIXER 712..896 | FILTER 902..1236 | RIGHT 1242..1552
# Rows: r1 y10..130 | r2 y138..258 | r3 y266..404. FILTER box spans r1+r2.

# --- POLY-MOD (A, r1) ---
box(8, 10, 292, 120, "POLY-MOD")
knob(60, 60, "fil_env")
knob(124, 60, "osc2")
body.append('<text class="lbl" x="60" y="94">FIL. ENV.</text><text class="lbl" x="124" y="94">OSC 2</text>')
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
     extra='<text class="lbl" x="-24" y="40">LFO</text><text class="lbl" x="26" y="40">NOISE</text><text class="lbl" y="56">SOURCE MIX</text>')
for x, lab in ((128, "FREQ 1"), (166, "FREQ 2"), (204, "PW 1"), (242, "PW 2"), (280, "FILTER")):
    btn(x, 330, f"wm_{lab.lower().replace(' ', '')}", label=lab, label_above=-24)
subhead(204, 380, "DESTINATION", 10)

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

# --- GLIDE / UNISON (B, r3, unbordered) ---
body.append('<line class="rule" x1="330" y1="272" x2="330" y2="398"/>')
knob(372, 332, "glide", label="GLIDE", label_dy=38)
btn(444, 332, "unison", label="UNISON", label_dy=38)

# --- MIXER (r1) ---
box(712, 10, 184, 120, "MIXER")
knob(756, 64, "mix_osc1", label="OSC 1")
knob(804, 64, "mix_osc2", label="OSC 2")
knob(852, 64, "mix_noise", label="NOISE")

# --- FILTER (tall r1+r2) ---
box(902, 10, 334, 248, "FILTER")
btn(928, 62, "flt_preset", color="btnGray", label="PRESET")
knob(988, 58, "flt_cutoff", label="CUTOFF")
knob(1060, 58, "flt_reso", label="RESONANCE")
knob(1140, 58, "flt_envamt", label="ENVELOPE")
body.append('<text class="lbl" x="1140" y="100">AMOUNT</text>')
btn(1204, 62, "flt_keyboard", label="KEYBOARD")
knob(952, 196, "flt_a", label="ATTACK", label_dy=42)
knob(1018, 196, "flt_d", label="DECAY", label_dy=42)
knob(1084, 196, "flt_s", label="SUSTAIN", label_dy=42)
knob(1152, 196, "flt_r", label="RELEASE", label_dy=42)

# --- MASTER TUNE / VOLUME (white knobs, unbordered, right) ---
knob(1330, 66, "master_tune", label="MASTER TUNE", white=True, label_dy=40)
knob(1462, 66, "volume", label="VOLUME", white=True, label_dy=40)

# --- AMPLIFIER (right, r2) ---
box(1242, 138, 310, 120, "AMPLIFIER")
knob(1292, 196, "amp_a", label="ATTACK", label_dy=44)
knob(1360, 196, "amp_d", label="DECAY", label_dy=44)
knob(1428, 196, "amp_s", label="SUSTAIN", label_dy=44)
knob(1494, 196, "amp_r", label="RELEASE", label_dy=44)

# --- PROGRAMMER (r3, spans B..FILTER columns) ---
box(476, 266, 760, 138, "PROGRAMMER")
btn(500, 330, "prog_preset", color="btnGray", label="PRESET", label_dy=36)
btn(542, 330, "prog_record", color="btnOrange", label="RECORD", label_dy=36)
btn(584, 330, "prog_bankselect", color="btnGray", led=False)
body.append('<text class="lbl" x="584" y="356">BANK</text><text class="lbl" x="584" y="368">SELECT</text>')
body.append('<g class="display" data-param="display"><rect x="614" y="314" width="80" height="32" rx="2" fill="#2b0d0d" stroke="#7a7a7a" stroke-width="1"/>'
            '<text class="lbl" x="632" y="364">BANK</text><text class="lbl" x="676" y="364">PROGRAM</text></g>')
for i in range(8):
    btn(742 + i * 42, 330, f"program_{i+1}", color="btnGray", num_above=str(i + 1))
subhead(889, 376, "PROGRAM SELECT", 14)

# --- RELEASE / EDIT / TUNE (r3 right, unbordered) ---
btn(1288, 330, "release_btn", color="btnDark", label="RELEASE", label_dy=36)
btn(1372, 330, "edit_btn", color="btnGray", led=False, label="EDIT", label_dy=36)
btn(1444, 330, "tune_btn", color="btnGray", led=False, label="TUNE", label_dy=36)

# ================= assemble =================
svg = f'''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     viewBox="0 0 {PW} {PH}" width="{PW}" height="{PH}">
  <style>
    text {{ font-family: Helvetica, Arial, sans-serif; fill: #f0f0f0; text-anchor: middle; }}
    .hdr   {{ font-size: 11px; font-weight: 600; letter-spacing: 1.6px; }}
    .lbl   {{ font-size: 7.6px; letter-spacing: 0.8px; }}
    .sub   {{ font-size: 7.2px; letter-spacing: 1.4px; }}
    .num   {{ font-size: 8.5px; letter-spacing: 0.5px; }}
    .scale {{ font-size: 5.1px; fill: #e8e8e8; letter-spacing: 0; stroke: none; }}
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
with open(OUT, "w") as fh:
    fh.write(svg)
print("wrote", OUT, len(svg), "bytes")
