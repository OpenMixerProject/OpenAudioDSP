/* SPDX-License-Identifier: GPL-3.0-only */
#include "audio.h"
#include "openaudiodsp/compressor.h"
#include "openaudiodsp/eq.h"
#include "openaudiodsp/gate.h"

static const int bands = 4;
static const float q28 = 268435456.0f;

// Bench settings; change only while stopped. WDSP is read-only on these cores.
sGate wingGates[WING_STRIPS];
bool wingEqEnabled[WING_USED_SLOTS];
float pm wingEqCoefficients[WING_USED_SLOTS * 5 * bands];
bool wingCompEnabled[WING_USED_SLOTS];
sCompressor wingCompressor[WING_USED_SLOTS];

static sGateState gateState[WING_STRIPS];
static float eqState[WING_USED_SLOTS * 2 * bands];
static sCompressorState compState[WING_USED_SLOTS];
static float samples[WING_USED_SLOTS][WING_BLOCK];

static bool enabled(int slot) {
    return wingEqEnabled[slot] || wingCompEnabled[slot] ||
        (slot < WING_STRIPS && wingGates[slot].value_enable);
}

static int32_t toWire(float sample) {
    if (sample >= 8.0f) return INT32_MAX;
    if (sample <= -8.0f) return INT32_MIN;
    if (sample != sample) return 0;
    return (int32_t)(sample * q28);
}

void wingProcessStrips(const WingRxBlock rx, WingTxBlock tx) {
    for (int slot = 0; slot < WING_USED_SLOTS; ++slot) {
        if (!enabled(slot)) continue;
        const int32_t *words = &rx[slot / WING_LANE_SLOTS][slot % WING_LANE_SLOTS];
        for (int n = 0; n < WING_BLOCK; ++n) samples[slot][n] = words[n * WING_LANE_SLOTS] / q28;
    }

    // Returns are after the gate range, but have their own EQ and compressor.
    gateProcessBatch(samples, wingGates, gateState, WING_STRIPS);
    eqProcessBatch(samples, wingEqEnabled, wingEqCoefficients, eqState, WING_USED_SLOTS, bands);
    compressorProcessBatch(samples, samples, wingCompressor, compState, wingCompEnabled, WING_USED_SLOTS, 1.0f);

    for (int slot = 0; slot < WING_USED_SLOTS; ++slot) {
        if (!enabled(slot)) continue;
        int32_t *words = &tx[slot / WING_LANE_SLOTS][slot % WING_LANE_SLOTS];
        for (int n = 0; n < WING_BLOCK; ++n) words[n * WING_LANE_SLOTS] = toWire(samples[slot][n]);
    }
}
