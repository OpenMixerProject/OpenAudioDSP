/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef OPENAUDIODSP_GATE_H
#define OPENAUDIODSP_GATE_H

#include <stdbool.h>

#define OPENAUDIODSP_GATE_BLOCK_SAMPLES 16

#ifdef __cplusplus
extern "C" {
#endif

/* Unified Hard Gate (value_expanderSlope == 0.0f) and Downward Expander
 * (value_expanderSlope = ratio - 1.0f, e.g. 1.0=2:1, 2.0=3:1, 3.0=4:1).
 * value_thresholdSq16 = 16 * threshold_linear^2 for zero-sqrt Hard Gate compare.
 * value_gainmin = 10^(-rangeDb / 20) sets the Hard Gate attenuation floor.
 * value_thresholdDb and value_negRangeDb (-rangeDb) drive Expander mode.
 * hold_ticks is converted to int on attack and counts blocks, not samples. */
typedef struct {
    bool value_enable;
    float value_thresholdSq16;
    float value_gainmin;
    float value_coeff_attack;
    float value_hold_ticks;
    float value_coeff_release;
    float value_thresholdDb;
    float value_negRangeDb;
    float value_expanderSlope;
} sGate;

typedef struct {
    float envelope;
    int holdTimer;
} sGateState;

/* Process contiguous channels in place; channelCount must be nonnegative.
 * Caller owns independent settings/state per channel, disjoint from samples.
 * Disabled channels preserve both samples and state; settings are read-only.
 * inputScale is positive and converts sample amplitude to dBFS reference scale
 * (1.0f for normalized samples; X32 uses INT32_TO_FLOAT_NORM).
 * Initialize states before use; this function neither allocates nor resets them.
 * Zero channels accesses no pointers. Coefficients must be in [0, 1], hold_ticks
 * nonnegative and representable as int, and initial envelopes in [0, 1]. */
void gateProcessBatch(float samples[][OPENAUDIODSP_GATE_BLOCK_SAMPLES],
                      const sGate settings[], sGateState states[],
                      int channelCount, float inputScale);

#ifdef __cplusplus
}
#endif

#endif
