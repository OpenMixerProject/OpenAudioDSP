/* SPDX-License-Identifier: GPL-3.0-only */
#include "audio.h"

typedef char ActiveSlotsFitBothDirections[
    WING_USED_SLOTS <= WING_RX_LANES * WING_LANE_SLOTS &&
    WING_USED_SLOTS <= WING_TX_LANES * WING_LANE_SLOTS ? 1 : -1];

#if WING_DSP != 4
void wingProcessStrips(const WingRxBlock rx, WingTxBlock tx);
#endif

void wingAudioProcess(const WingRxBlock rx, WingTxBlock tx) {
    // Bit-exact bypass (including LSBs that a binary32 round trip would lose).
    for (int slot = 0; slot < WING_USED_SLOTS; ++slot) {
        int lane = slot / WING_LANE_SLOTS, offset = slot % WING_LANE_SLOTS;
        for (int n = offset; n < WING_LANE_WORDS; n += WING_LANE_SLOTS)
            tx[lane][n] = rx[lane][n];
    }
    for (int slot = WING_USED_SLOTS; slot < WING_TX_LANES * WING_LANE_SLOTS; ++slot) {
        int lane = slot / WING_LANE_SLOTS, offset = slot % WING_LANE_SLOTS;
        for (int n = offset; n < WING_LANE_WORDS; n += WING_LANE_SLOTS)
            tx[lane][n] = 0;
    }
#if WING_DSP != 4
    wingProcessStrips(rx, tx);
#endif
}
