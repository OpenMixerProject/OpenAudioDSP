/* SPDX-License-Identifier: GPL-3.0-only */
#include "openaudiodsp/compressor.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void test_rms_attack_and_ranges(void) {
    float input[5][16], original[5][16], output[5][16];
    sCompressor settings[5], originalSettings[5];
    sCompressorState states[5];
    for (int c = 0; c < 5; c++) {
        settings[c] = (sCompressor){-12.0f, 0.75f, 0.5f + 0.25f * c,
                                    0.5f, 2.75f, 0.75f, 0.0f, 0.0f};
        states[c] = (sCompressorState){1.0f, 7};
        for (int s = 0; s < 16; s++) {
            input[c][s] = c == 1 ? (s == 0 ? 4.0f : 0.0f) :
                (c == 2 && s % 2 ? -1.0f : 1.0f);
            output[c][s] = 37.0f;
        }
    }
    memcpy(original, input, sizeof(input));
    memcpy(originalSettings, settings, sizeof(settings));
    const bool enabled[] = {true, false, true};
    compressorProcessBatch(input + 1, output + 1, settings + 1, states + 1, enabled, 3, 1.0f);
    assert(memcmp(original, input, sizeof(input)) == 0);
    assert(memcmp(originalSettings, settings, sizeof(settings)) == 0);

    /* RMS=1 (0 dBFS), threshold=-12 dB, slope=0.75 => -9 dB gain reduction (~0.354813). */
    const float expectedTarget = powf(10.0f, -9.0f / 20.0f);
    for (int c = 0; c < 5; c++) {
        if (c == 0 || c == 2 || c == 4) {
            assert(states[c].envelope == 1.0f && states[c].holdTimer == 7);
            for (int s = 0; s < 16; s++) {
                assert(output[c][s] == (c == 2 ? input[c][s] : 37.0f));
            }
        } else {
            /* Fast approximation within 0.03 dB of ideal 10^(-9/20) */
            float targetErrDb = fabsf(20.0f * log10f(states[c].envelope / expectedTarget));
            assert(targetErrDb < 0.05f);
            assert(states[c].holdTimer == 2);
            /* Verify per-sample monotonic decay towards target */
            float prevAbs = fabsf(output[c][0]);
            if (c == 3) {
                for (int s = 1; s < 16; s++) {
                    assert(fabsf(output[c][s]) <= prevAbs + 1e-6f);
                    prevAbs = fabsf(output[c][s]);
                }
            }
        }
    }
    compressorProcessBatch(NULL, NULL, NULL, NULL, NULL, 0, 1.0f);
}

static void test_hold_release_and_equality(void) {
    float input[1][16], output[1][16];
    sCompressor settings = {0.0f, 0.75f, 2.0f, 0.25f, 4.75f, 0.5f, 0.0f, 0.0f};
    sCompressorState state = {0.25f, 2};
    for (int s = 0; s < 16; s++) input[0][s] = s % 2 ? -0.25f : 0.25f;

    for (int block = 0; block < 3; block++) {
        compressorProcessBatch(input, output, &settings, &state, NULL, 1, 1.0f);
        for (int s = 0; s < 16; s++) {
            float envelope = block < 2 ? 0.25f : 1.0f - ldexpf(0.75f, -(s + 1));
            assert(fabsf(output[0][s] - input[0][s] * envelope * 2.0f) < 1e-6f);
        }
        assert(state.holdTimer == (block == 0 ? 1 : 0));
    }
    assert(fabsf(state.envelope - (1.0f - ldexpf(0.75f, -16))) < 1e-6f);

    /* Target equals envelope: do not reload hold, even with nonzero attack. */
    state = (sCompressorState){1.0f, 2};
    for (int block = 0; block < 3; block++) {
        compressorProcessBatch(input, output, &settings, &state, NULL, 1, 1.0f);
        assert(state.envelope == 1.0f);
        assert(state.holdTimer == (block == 0 ? 1 : 0));
    }

    /* Silence stays finite and permits release rather than freezing the state. */
    memset(input, 0, sizeof(input));
    state = (sCompressorState){0.5f, 0};
    compressorProcessBatch(input, output, &settings, &state, NULL, 1, 1.0f);
    assert(fabsf(state.envelope - (1.0f - ldexpf(0.5f, -16))) < 1e-6f);
    for (int s = 0; s < 16; s++) assert(output[0][s] == 0.0f);
}

static void test_scaling_in_place_and_controls(void) {
    float input[1][16], raw[1][16], inPlace[1][16], output[1][16], rawOutput[1][16];
    sCompressor settings = {-12.0f, 0.75f, 1.25f, 0.5f, 1.75f, 1.0f, 0.0f, 0.0f};
    sCompressorState state = {1.0f, 0}, rawState = state, aliasState = state;
    for (int s = 0; s < 16; s++) {
        input[0][s] = s % 2 ? -0.5f : 0.5f;
        raw[0][s] = ldexpf(input[0][s], 31);
    }
    memcpy(inPlace, input, sizeof(input));
    const bool disabled = false;
    compressorProcessBatch(inPlace, inPlace, &settings, &aliasState, &disabled, 1, 1.0f);
    assert(memcmp(input, inPlace, sizeof(input)) == 0);
    assert(aliasState.envelope == 1.0f && aliasState.holdTimer == 0);
    compressorProcessBatch(input, output, &settings, &state, NULL, 1, 1.0f);
    compressorProcessBatch(raw, rawOutput, &settings, &rawState, NULL, 1, 0x1p-31f);
    compressorProcessBatch(inPlace, inPlace, &settings, &aliasState, NULL, 1, 1.0f);
    assert(state.envelope == rawState.envelope && state.holdTimer == rawState.holdTimer);
    assert(state.envelope == aliasState.envelope && state.holdTimer == aliasState.holdTimer);
    assert(memcmp(output, inPlace, sizeof(output)) == 0);
    for (int s = 0; s < 16; s++) assert(rawOutput[0][s] == ldexpf(output[0][s], 31));

    settings.value_1_minus_1_by_ratio = 0.0f;
    settings.value_makeup = 0.25f;
    state.holdTimer = 0;
    compressorProcessBatch(input, output, &settings, &state, NULL, 1, 1.0f);
    assert(state.envelope == 1.0f);
    for (int s = 0; s < 16; s++) assert(output[0][s] == input[0][s] * 0.25f);
}

static void test_zero_ms_peak_lookahead_and_soft_knee(void) {
    /* 1. True 0ms attack: single-sample 0 dBFS spike at s=12, T=-18 dB, 99:1, Hard knee */
    float input[1][16], output[1][16];
    memset(input, 0, sizeof(input));
    input[0][12] = 1.0f;
    float slope = 1.0f - (1.0f / 99.0f);
    sCompressor settings = {-18.0f, slope, 1.0f, 1.0f, 0.0f, 0.05f, 0.0f, 0.0f};
    sCompressorState state = {1.0f, 0};

    compressorProcessBatch(input, output, &settings, &state, NULL, 1, 1.0f);
    float outDb = 20.0f * log10f(fabsf(output[0][12]));
    /* Expected: -18 + (0 - (-18)) / 99 = -17.818 dBFS (zero transient overshoot!) */
    assert(fabsf(outDb - (-17.818f)) < 0.05f);

    /* 2. 6-stage quadratic soft-knee (0, 6, 12, 18, 26, 36 dB) at threshold (-18 dBFS) */
    const float kneeWidths[6] = {0.0f, 6.0f, 12.0f, 18.0f, 26.0f, 36.0f};
    float threshLin = powf(10.0f, -18.0f / 20.0f);
    for (int s = 0; s < 16; s++) input[0][s] = threshLin;

    float prevEnv = 1.01f;
    for (int k = 0; k < 6; k++) {
        float W = kneeWidths[k];
        settings.value_kneeHalf = 0.5f * W;
        settings.value_kneeCoeff = (W > 0.0f) ? (-slope / (2.0f * W)) : 0.0f;
        state = (sCompressorState){1.0f, 0};
        compressorProcessBatch(input, output, &settings, &state, NULL, 1, 1.0f);

        float expectedGrDb = (W > 0.0f) ? (-slope * W / 8.0f) : 0.0f;
        float actualGrDb = 20.0f * log10f(state.envelope);
        assert(fabsf(actualGrDb - expectedGrDb) < 0.08f);
        assert(state.envelope <= prevEnv);
        prevEnv = state.envelope;
    }
}

int main(void) {
    test_rms_attack_and_ranges();
    test_hold_release_and_equality();
    test_scaling_in_place_and_controls();
    test_zero_ms_peak_lookahead_and_soft_knee();
    puts("Compressor batch tests passed");
    return 0;
}
