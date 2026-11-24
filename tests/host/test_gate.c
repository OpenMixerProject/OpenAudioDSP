/* SPDX-License-Identifier: GPL-3.0-only */
#include "openaudiodsp/gate.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static sGate makeHardGate(bool enable, float thrLin, float gainMin,
                          float attack, float hold, float release) {
    sGate g;
    g.value_enable = enable;
    g.value_thresholdSq16 = 16.0f * thrLin * thrLin;
    g.value_gainmin = gainMin;
    g.value_coeff_attack = attack;
    g.value_hold_ticks = hold;
    g.value_coeff_release = release;
    g.value_thresholdDb = -80.0f;
    g.value_negRangeDb = -60.0f;
    g.value_expanderSlope = 0.0f;
    return g;
}

static void test_rms_bypass_and_ranges(void) {
    float samples[5][OPENAUDIODSP_GATE_BLOCK_SAMPLES];
    float original[5][OPENAUDIODSP_GATE_BLOCK_SAMPLES];
    sGate settings[5] = {
        makeHardGate(true, 0.1f, 0.0f, 1.0f, 0.0f, 1.0f),
        makeHardGate(true, 0.6f, 0.0f, 1.0f, 0.0f, 1.0f),
        makeHardGate(true, 0.8f, 0.25f, 1.0f, 0.0f, 1.0f), /* gainmin=0.25 (-12 dB range) */
        makeHardGate(false, 0.1f, 0.0f, 1.0f, 0.0f, 1.0f),
        makeHardGate(true, 0.1f, 0.0f, 1.0f, 0.0f, 1.0f)
    };
    sGate originalSettings[5];
    sGateState states[5] = {{0.25f, 4}, {0.0f, 0}, {1.0f, 0}, {0.75f, 3}, {0.5f, 5}};

    for (int channel = 0; channel < 5; channel++) {
        for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
            /* RMS = sqrt(0.5) ~= 0.7071, distinct from mean (0.5) and peak (1). */
            samples[channel][sample] = sample < 8 ? 1.0f : 0.0f;
        }
    }
    memcpy(original, samples, sizeof(samples));
    memcpy(originalSettings, settings, sizeof(settings));

    gateProcessBatch(&samples[1], &settings[1], &states[1], 3, 1.0f);
    assert(memcmp(originalSettings, settings, sizeof(settings)) == 0);
    for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
        assert(samples[0][sample] == original[0][sample]);
        assert(samples[1][sample] == original[1][sample]);
        assert(samples[2][sample] == original[2][sample] * 0.25f); /* respects Range! */
        assert(samples[3][sample] == original[3][sample]);
        assert(samples[4][sample] == original[4][sample]);
    }
    assert(states[0].envelope == 0.25f && states[0].holdTimer == 4);
    assert(states[1].envelope == 1.0f && states[1].holdTimer == 0);
    assert(states[2].envelope == 0.25f && states[2].holdTimer == 0);
    assert(states[3].envelope == 0.75f && states[3].holdTimer == 3);
    assert(states[4].envelope == 0.5f && states[4].holdTimer == 5);

    /* Exact threshold equality opens the gate, including at zero hold time. */
    settings[2].value_thresholdSq16 = 16.0f * 0.5f * 0.5f;
    for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
        samples[2][sample] = 0.5f;
    }
    gateProcessBatch(&samples[2], &settings[2], &states[2], 1, 1.0f);
    for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
        assert(samples[2][sample] == 0.5f);
    }

    gateProcessBatch(NULL, NULL, NULL, 0, 1.0f);
}

static void test_attack_hold_release(void) {
    float samples[1][OPENAUDIODSP_GATE_BLOCK_SAMPLES];
    sGate settings = makeHardGate(true, 0.5f, 0.0f, 0.5f, 2.75f, 0.75f);
    sGateState state = {0.0f, 0};

    for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
        samples[0][sample] = sample % 2 ? -1.0f : 1.0f;
    }
    gateProcessBatch(samples, &settings, &state, 1, 1.0f);
    for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
        float envelope = 1.0f - ldexpf(1.0f, -(sample + 1));
        assert(samples[0][sample] == (sample % 2 ? -envelope : envelope));
    }
    float heldEnvelope = 1.0f - ldexpf(1.0f, -16);
    assert(state.envelope == heldEnvelope && state.holdTimer == 2);

    for (int block = 0; block < 3; block++) {
        for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
            samples[0][sample] = 0.25f;
        }
        gateProcessBatch(samples, &settings, &state, 1, 1.0f);
        for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
            float envelope = block < 2 ? heldEnvelope : ldexpf(heldEnvelope, -2 * (sample + 1));
            assert(samples[0][sample] == 0.25f * envelope);
        }
        assert(state.holdTimer == (block == 0 ? 1 : 0));
    }
    assert(state.envelope == ldexpf(heldEnvelope, -32));

    settings.value_coeff_attack = 1.0f;
    settings.value_hold_ticks = 4.0f;
    for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
        samples[0][sample] = -1.0f;
    }
    gateProcessBatch(samples, &settings, &state, 1, 1.0f);
    assert(state.envelope == 1.0f && state.holdTimer == 4);
    for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
        assert(samples[0][sample] == -1.0f);
    }
}

static void test_expander_modes_and_floor(void) {
    float samples[1][OPENAUDIODSP_GATE_BLOCK_SAMPLES];
    /* Threshold = -20 dBFS, Range = 24 dB, Exp 2:1 (slope 1.0), instant release */
    sGate settings = {true, 0.0f, 0.063f, 1.0f, 0.0f, 1.0f, -20.0f, -24.0f, 1.0f};
    sGateState state = {1.0f, 0};

    /* Input = -30 dBFS (10 dB below threshold) => Exp 2:1 adds -10 dB attenuation */
    float amp = powf(10.0f, -30.0f / 20.0f);
    for (int s = 0; s < OPENAUDIODSP_GATE_BLOCK_SAMPLES; s++) samples[0][s] = amp;
    gateProcessBatch(samples, &settings, &state, 1, 1.0f);
    float attenDb = 20.0f * log10f(state.envelope);
    assert(fabsf(attenDb - (-10.0f)) < 0.06f);

    /* Exp 3:1 (slope 2.0) at -30 dBFS => -20 dB attenuation */
    settings.value_expanderSlope = 2.0f;
    state = (sGateState){1.0f, 0};
    for (int s = 0; s < OPENAUDIODSP_GATE_BLOCK_SAMPLES; s++) samples[0][s] = amp;
    gateProcessBatch(samples, &settings, &state, 1, 1.0f);
    attenDb = 20.0f * log10f(state.envelope);
    assert(fabsf(attenDb - (-20.0f)) < 0.06f);

    /* Exp 4:1 (slope 3.0) at -30 dBFS => -30 dB, clamped at -rangeDb = -24 dB */
    settings.value_expanderSlope = 3.0f;
    state = (sGateState){1.0f, 0};
    for (int s = 0; s < OPENAUDIODSP_GATE_BLOCK_SAMPLES; s++) samples[0][s] = amp;
    gateProcessBatch(samples, &settings, &state, 1, 1.0f);
    attenDb = 20.0f * log10f(state.envelope);
    assert(fabsf(attenDb - (-24.0f)) < 0.06f);
}

int main(void) {
    test_rms_bypass_and_ranges();
    test_attack_hold_release();
    test_expander_modes_and_floor();
    puts("Gate batch tests passed");
    return 0;
}
