# Native-float engine experiment (Oct 2026) — measured, rejected, archived

## What this is
A complete conversion of the Muhammad engine signal path from the ported
fixed-point/int32 chain to native float32: float process()/mixR()/lastMix()
buffers, unit-domain envelopes (track ADSR out of Q24), the per-voice
int32 quantiser deleted, the DC coupling cap as a float one-pole
(dcA_ = dcK/2^30), float pan gains, plugin scratch float, renderChunk
dropping the /32768 law. This patch applies on the `native-float` branch
tip = commit f56f46b (setReleaseCut + quantnoise probe, still INT kernel).

## How to reach the base state and apply
    git fetch            # base = commit f56f46b (tag of the experiment branch)
    git checkout f56f46b
    git apply experiments/native-float-experiment.patch
    mkdir build && cd build
    cmake .. -DMELODEE_DIR=/home/user/build/melodee && make -j8

## The measurements that decided it (all real, rerunnable)
Bench: 6-round INTERLEAVED medians, int vs float on identical ISA settings
(`./build/bench_engine` alternating; single runs swing ~8% from governor):
  float vs int: 8v scalar -0.1..+4.4%  (int wins the 1v/scalar path,
  tie at polyphony; AVX2+FMA halves BOTH equally: 414->205 wide, 798->376 os2)
Noise: `./build/test_quantnoise 80` (long-release saw, 1 s tail FFT):
  int   floor -123.5 dB, max off-harmonic spur -88.4 dBFS @ 1.5*f0
  float floor -123.1 dB, max off-harmonic spur -87.7 dBFS @ 1.5*f0
  => the spur is engine-intrinsic (BLEP), NOT quantization; the per-voice
  15-bit quantiser was phase-dithered across the 8-voice mix. A/B of float
  vs firmware: SNR 67-80 dB, max 7 dlsb (purely the firmware's own
  truncation, bounded, non-accumulating).

## Verdict
Kept the int kernel: zero float gain, preserves the free forever-gate
(A/B bit-exact vs firmware), and the true 2x came from actually emitting
AVX2 (dispatch shipped in v1.10.0-avx2). If the engine ever moves to a
pure-float DSP (new filter topologies, per-voice state in float), this
patch is the worked example of the fixed-point->float migration path.

## Also contained (worth keeping if reapplying)
The float variant's release cut is already upstream via setReleaseCut();
nothing else in the patch is still missing from master.
