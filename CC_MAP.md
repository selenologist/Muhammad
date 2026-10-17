# Muhammad - MIDI Continuous Controller Map

Complete, live mapping implemented in `src/muhammad_plugin.cpp`
(`ccRawParam()` + the CC switch in the event path). Raw-byte targets are the
Prophet-5 Rev-4 wire patch bytes; "blob" targets are controls that exist
outside the 133-byte wire patch (they persist in the plugin's session state,
and FEEDBACK/DRIFT persist in saved `.muhammad` files on their extension
bytes). Rev4-family assignments follow Sequential's Prophet-5 MIDI
Implementation Worksheet; every entry marked **extension** uses a channel
voice-mode CC that is unassigned in both the Rev4 implementation and the
General-MIDI controller table, chosen to avoid conflicting with anything a
GM sequencer sends.

## Special / engine-behaviour CCs

| CC | Function | target | origin |
|----|----------|--------|--------|
| 1 | MOD WHEEL (vibrato depth; takes over LFO INIT) | engine | Rev4 impl 1.4 |
| 2 | MPE Range Control / bend range (semitones, per ch) | engine | MPE |
| 7 | VOLUME (post-fader, multiplies master trim) | — | Rev4 impl 1.4 |
| 13 | EXPRESSION (multiplicative; Rev4 uses 13, not GM 11) | — | Rev4 impl 1.4 |
| 0 / 32 | BANK SELECT coarse/fine (panel cursor) | — | standard |
| 6 / 38 | NRPN data increment / parameter increment (+100/101 RPN) | — | standard |
| 64 | DAMPER (sustain pedal, gated note-off) | — | Rev4 impl 1.4 |
| 66/68/69 | soft/hold1/hold2 — accepted, ignored | — | — |
| 74 | BRIGHTNESS (adds to cutoff) | engine | Rev4 CC74 |
| 85 | DRIFT (thermal-walk depth; Rev4's VINTAGE CC) | byte 37 | Rev4 impl 1.4 |
| 42 | **FEEDBACK** (VCA->filter loop, 0..127) | byte 133 | **extension** |
| 43 | **PAN WIDTH** (Oberheim-style voice spread) | blob | **extension** |
| 44 | **OVERSAMPLE 2x** toggle (value >= 64 = on) | blob | **extension** |
| 45 | **MASTER TUNE** bipolar: 64 = 0, +-63 -> +-100 cents | blob | **extension** |
| 48 | **OUTPUT TRIM** 0..2 steps: 0-42/43-84/85-127 | blob | **extension** |
| 100/101 | RPN select (RPN0 = bend range up/down per channel) | — | standard |
| 120 / 123 | ALL SOUND OFF / ALL OFF (panic) | — | standard |
| 121 | RESET: volume+expression to 127, brightness 0 | — | standard |

## Wire-parameter CCs (Rev4 implementation 1.4)

| CC | Parameter | wire byte | origin |
|----|-----------|-----------|--------|
| 3 | OSC A COARSE | 0 | Rev4 impl 1.4 |
| 9 | OSC B COARSE | 1 | Rev4 impl 1.4 |
| 14 | OSC B FINE | 2 | Rev4 impl 1.4 |
| 15 | OSC A SAW | 3 | Rev4 impl 1.4 |
| 20 | OSC A PULSE | 4 | Rev4 impl 1.4 |
| 21 | OSC A PULSE W | 8 | Rev4 impl 1.4 |
| 22 | OSC B PULSE W | 9 | Rev4 impl 1.4 |
| 23 | SYNC | 10 | Rev4 impl 1.4 |
| 24 | OSC B LOW | 11 | Rev4 impl 1.4 |
| 25 | OSC B KEYBD | 12 | Rev4 impl 1.4 |
| 26 | GLIDE | 13 | Rev4 impl 1.4 |
| 27 | OSC A LEVEL | 14 | Rev4 impl 1.4 |
| 28 | OSC B LEVEL | 15 | Rev4 impl 1.4 |
| 29 | NOISE | 16 | Rev4 impl 1.4 |
| 30 | OSC B SAW | 5 | Rev4 impl 1.4 |
| 31 | RESONANCE | 18 | Rev4 impl 1.4 |
| 35 | KEYBD>FLT | 19 | Rev4 impl 1.4 |
| 41 | FILTER MODEL | 20 | Rev4 impl 1.4 |
| 46 | LFO RATE | 21 | Rev4 impl 1.4 |
| 47 | LFO INIT | 22 | Rev4 impl 1.4 |
| 52 | OSC B TRI | 6 | Rev4 impl 1.4 |
| 53 | WHEEL MIX | 26 | Rev4 impl 1.4 |
| 54 | WHEEL>OSCA | 27 | Rev4 impl 1.4 |
| 55 | WHEEL>OSCB | 28 | Rev4 impl 1.4 |
| 56 | PW WHEEL A | 29 | Rev4 impl 1.4 |
| 57 | PW WHEEL B | 30 | Rev4 impl 1.4 |
| 58 | WHEEL>FLT | 31 | Rev4 impl 1.4 |
| 59 | POLYMOD AMT | 32 | Rev4 impl 1.4 |
| 60 | POLYMOD B | 33 | Rev4 impl 1.4 |
| 61 | POLYMOD>OSCA | 34 | Rev4 impl 1.4 |
| 62 | POLYMOD>PW | 35 | Rev4 impl 1.4 |
| 63 | POLYMOD>FLT | 36 | Rev4 impl 1.4 |
| 70 | BEND RANGE | 86 | Rev4 impl 1.4 |
| 71 | RETRIGGER | 87 | Rev4 impl 1.4 |
| 73 | CUTOFF | 17 | Rev4 impl 1.4 |
| 86 | PRESS>FLT | 38 | Rev4 impl 1.4 |
| 87 | PRESS>LFO | 39 | Rev4 impl 1.4 |
| 89 | ENV>FLT | 40 | Rev4 impl 1.4 |
| 90 | VEL>FLT | 41 | Rev4 impl 1.4 |
| 102 | VEL>AMP | 42 | Rev4 impl 1.4 |
| 103 | FLT ATTACK | 43 | Rev4 impl 1.4 |
| 104 | AMP ATTACK | 44 | Rev4 impl 1.4 |
| 105 | FLT DECAY | 45 | Rev4 impl 1.4 |
| 106 | AMP DECAY | 46 | Rev4 impl 1.4 |
| 107 | FLT SUSTAIN | 47 | Rev4 impl 1.4 |
| 108 | AMP SUSTAIN | 48 | Rev4 impl 1.4 |
| 109 | FLT RELEASE | 49 | Rev4 impl 1.4 |
| 110 | AMP RELEASE | 50 | Rev4 impl 1.4 |
| 111 | RELEASE ON | 51 | Rev4 impl 1.4 |
| 112 | UNISON | 52 | Rev4 impl 1.4 |
| 113 | UNISON CNT | 53 | Rev4 impl 1.4 |
| 114 | UNISON DET | 54 | Rev4 impl 1.4 |
| 116 | OSC B PULSE | 7 | Rev4 impl 1.4 |
| 117 | LFO SAW | 23 | Rev4 impl 1.4 |
| 118 | LFO TRI | 24 | Rev4 impl 1.4 |
| 119 | LFO PULSE | 25 | Rev4 impl 1.4 |

Notes
-----
* CC85 is the Rev4 "VINTAGE" controller; Muhammad's DRIFT inherits that
  name's slot, so the CC carries over honestly.
* CC70/CC71 (BEND RANGE / RETRIGGER) previously aliased the wrong parameter
  internally; fixed alongside FEEDBACK (byte numbers were used where clap
  parameter IDs are required).
* RPN0 (MSB 101=0, LSB 100=0) then NRPN-style data via CC6/CC38, or MPE CC2,
  set per-channel bend ranges; the panel BEND UP/DOWN rails also write them.
* Values arriving on a wire byte are clamped to that byte's panel law
  (0..127 unless the byte carries a documented wider meaning).
