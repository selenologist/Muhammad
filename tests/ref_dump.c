/* Reference dump: renders tests/scenario.h through the melodee firmware's own
 * hostsim (the same sources as the device), writing the part's int32 blocks to
 * stdout for the port's A/B test. Built with -DMELODEE_DIR. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scenario.h"

/* prophet_test.c's harness shape: the firmware's host build under a renamed main */
#define main hostsim_main
#include MELODEE_HOSTSIM_C
#undef main

int main(void)
{
    for (int s = 0; s < NSCEN; s++) {
        const struct Scenario sc = scenarios[s];
        memset(trk, 0, sizeof trk);
        eng_state_reset();
        memset(p5_ready, 0, sizeof p5_ready);
        host_tracks_init();
        host_preset(&trk[0], ENGI_PROPHET, (uint32_t)sc.preset);
        trk[0].p[P_VOICE] = V_POLY;
        trk[0].p[P_RESERVED34] = trk[0].p[P_RESERVED35] = trk[0].p[P_RESERVED36] = 0;
        if (sc.patchRes > 0) {                    /* closed mixer, hot filter */
            p5_patch[0].raw[P5_RESONANCE] = (uint8_t)sc.patchRes;
            p5_patch[0].raw[P5_CUTOFF] = (uint8_t)sc.patchCut;
            p5_patch[0].raw[P5_LEVEL_A] = p5_patch[0].raw[P5_LEVEL_B] = p5_patch[0].raw[P5_NOISE] = 0;
            p5_ready[0] = 1;
            p5_track_accept(&trk[0]);
        }
        unsigned nextEv = 0;
        for (unsigned blk = 0; blk < BLOCKS; blk++) {
            while (nextEv < sc.n && sc.ev[nextEv].block == blk) {
                if (sc.ev[nextEv].kind == 0) trk_note_on(&trk[0], (uint32_t)sc.ev[nextEv].a, (uint32_t)sc.ev[nextEv].b);
                else trk_note_off(&trk[0], (uint32_t)sc.ev[nextEv].a);
                nextEv++;
            }
            int32_t out[CTL];
            track_render(&trk[0], out, CTL);
            fwrite(out, 4, CTL, stdout);
        }
    }
    return 0;
}
