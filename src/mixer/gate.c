/* SPDX-License-Identifier: GPL-3.0-only
 * Unified Hard Gate (zero-sqrt power compare) + Downward Expander (2:1/3:1/4:1).
 * Copyright 2025-2026 OpenMixerProject.
 */
#include "openaudiodsp/gate.h"

#include <stdint.h>

#ifndef USE_SHARC_ASM_DYNAMICS
#ifdef __ADSP21000__
#define USE_SHARC_ASM_DYNAMICS 1
#else
#define USE_SHARC_ASM_DYNAMICS 0
#endif
#endif

#if defined(__ADSP21000__) && (USE_SHARC_ASM_DYNAMICS == 1)
#include "dynamics_asm.h"
#else

static inline float gateNormPowerToDb_fast(float normPower) {
    if (normPower <= 1.0e-14f) return -140.0f;

    union { float f; uint32_t i; } vx;
    vx.f = normPower;
    int32_t exp_val = (int32_t)((vx.i >> 23) & 0xFFu) - 127;
    vx.i = (vx.i & 0x007FFFFFu) | 0x3F800000u;
    float m = vx.f;
    float log2_m = (m - 1.0f) * (1.6797000f - 0.3398500f * m);
    return 3.01029995664f * ((float)exp_val + log2_m);
}

static inline float gateDbToLinear_fast(float dB) {
    if (dB <= -140.0f) return 0.0f;
    if (dB > 24.0f) dB = 24.0f;

    float x = dB * 0.16609640474f;
    int32_t xi = (int32_t)x;
    if ((float)xi > x) xi--;
    float xf = x - (float)xi;

    union { float f; uint32_t i; } scale;
    scale.i = (uint32_t)(xi + 127) << 23;
    return scale.f * (1.0f + xf * (0.65685425f + 0.34314575f * xf));
}
#endif

#pragma optimize_for_speed

void gateProcessBatch(float samples[][OPENAUDIODSP_GATE_BLOCK_SAMPLES],
                      const sGate settings[], sGateState states[],
                      int channelCount, float inputScale) {
    const float inputScaleSqDiv16 = 0.0625f * inputScale * inputScale;

    for (int channel = 0; channel < channelCount; channel++) {
        const sGate* gate = &settings[channel];
        if (!gate->value_enable) {
            continue;
        }

        float* restrict src_dst = samples[channel];

#if defined(__ADSP21000__) && (USE_SHARC_ASM_DYNAMICS == 1)
        sGateAsmParams gParams;
        gParams.thresholdSq16     = gate->value_thresholdSq16;
        gParams.gainMin           = gate->value_gainmin;
        gParams.coeffAttack       = gate->value_coeff_attack;
        gParams.holdTicks         = gate->value_hold_ticks;
        gParams.coeffRelease      = gate->value_coeff_release;
        gParams.thresholdDb       = gate->value_thresholdDb;
        gParams.negRangeDb        = gate->value_negRangeDb;
        gParams.expanderSlope     = gate->value_expanderSlope;
        gParams.env               = states[channel].envelope;
        gParams.holdTimer         = states[channel].holdTimer;
        gParams.inputScaleSqDiv16 = inputScaleSqDiv16;
        fxGateBlock_asm(src_dst, &gParams);
        states[channel].envelope  = gParams.env;
        states[channel].holdTimer = gParams.holdTimer;
#else
        float env = states[channel].envelope;
        int holdTimer = states[channel].holdTimer;
        float sumOfSquares = 0.0f;

        #pragma no_alias
        #pragma vector_for
        #pragma loop_count(OPENAUDIODSP_GATE_BLOCK_SAMPLES, OPENAUDIODSP_GATE_BLOCK_SAMPLES, OPENAUDIODSP_GATE_BLOCK_SAMPLES)
        for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
            sumOfSquares += src_dst[sample] * src_dst[sample];
        }

        float targetGainLinear = 1.0f;
        float expSlope = gate->value_expanderSlope;
        if (expSlope == 0.0f) {
            /* Hard Gate: zero-transcendental power compare, respects Range (value_gainmin) */
            targetGainLinear = (sumOfSquares < gate->value_thresholdSq16) ? gate->value_gainmin : 1.0f;
        } else {
            /* Downward Expander (2:1 / 3:1 / 4:1) clamped to Range */
            float levelDb = gateNormPowerToDb_fast(sumOfSquares * inputScaleSqDiv16);
            float diffDb = levelDb - gate->value_thresholdDb;
            if (diffDb < 0.0f) {
                float attenDb = diffDb * expSlope;
                if (attenDb < gate->value_negRangeDb) {
                    attenDb = gate->value_negRangeDb;
                }
                targetGainLinear = gateDbToLinear_fast(attenDb);
            }
        }

        float coeff = 0.0f;
        if (targetGainLinear > env) {
            coeff = gate->value_coeff_attack;
            holdTimer = (int)gate->value_hold_ticks;
        } else {
            if (holdTimer > 0) {
                holdTimer--;
            } else {
                coeff = gate->value_coeff_release;
            }
        }

        states[channel].holdTimer = holdTimer;

        float a1 = 1.0f - coeff;
        float b0 = coeff * targetGainLinear;

        #pragma no_alias
        #pragma loop_count(OPENAUDIODSP_GATE_BLOCK_SAMPLES, OPENAUDIODSP_GATE_BLOCK_SAMPLES, OPENAUDIODSP_GATE_BLOCK_SAMPLES)
        for (int sample = 0; sample < OPENAUDIODSP_GATE_BLOCK_SAMPLES; sample++) {
            env = (env * a1) + b0;
            src_dst[sample] *= env;
        }

        states[channel].envelope = env;
#endif
    }
}
