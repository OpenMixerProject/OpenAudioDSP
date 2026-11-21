/* SPDX-License-Identifier: GPL-3.0-only */
#include "audio.h"
#include "runtime.h"
#include "fxOverdrive.h"

static float fxIn[2][WING_BLOCK], fxOut[2][WING_BLOCK];
static float *fxInChannels[] = {fxIn[0], fxIn[1]};
static float *fxOutChannels[] = {fxOut[0], fxOut[1]};
static fxOverdrive overdrive(0, fxInChannels, fxOutChannels, 1);
static bool overdriveEnabled;

void wingAudioOverdrive(int enabled, float parameters[6]) {
    if (enabled) overdrive.rxData(parameters, 6);
    overdriveEnabled = enabled != 0;
}

void wingProcessOverdrive(const WingRxBlock rx, WingTxBlock tx) {
    if (!overdriveEnabled) return;
    // WING pair zero is lane 0 slots 0/1. Existing FX takes PCM32-scaled floats.
    for (int ch = 0; ch < 2; ++ch)
        for (int n = 0; n < WING_BLOCK; ++n)
            fxIn[ch][n] = rx[0][n * WING_LANE_SLOTS + ch] * 8.0f;
    overdrive.process();
    for (int ch = 0; ch < 2; ++ch)
        for (int n = 0; n < WING_BLOCK; ++n)
            tx[0][n * WING_LANE_SLOTS + ch] = (int32_t)(fxOut[ch][n] * 0.125f);
}
