/* SPDX-License-Identifier: GPL-3.0-only
 * Multi-stage soft-knee + 0ms peak-lookahead compressor kernel.
 * Copyright 2025-2026 OpenMixerProject.
 */
#include "openaudiodsp/compressor.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

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

/* Fast 10 * log10(normPower) without sqrtf(); max error < 0.026 dB. */
static inline float normPowerToDb_fast(float normPower) {
    if (normPower <= 1.0e-14f) return -140.0f;

    union { float f; uint32_t i; } vx;
    vx.f = normPower;
    int32_t exp_val = (int32_t)((vx.i >> 23) & 0xFFu) - 127;
    vx.i = (vx.i & 0x007FFFFFu) | 0x3F800000u; /* mantissa m in [1.0, 2.0) */
    float m = vx.f;
    float log2_m = (m - 1.0f) * (1.6797000f - 0.3398500f * m);
    return 3.01029995664f * ((float)exp_val + log2_m);
}

/* Fast 10^(dB/20) = 2^(dB / 6.0206) with quadratic mantissa refinement; max error < 0.028 dB. */
static inline float dbToLinear_fast(float dB) {
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

void compressorProcessBatch(
    float input[][OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES],
    float output[][OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES],
    const sCompressor* restrict settings, sCompressorState* restrict states,
    const bool enabled[], int channelCount, float inputScale) {
    const float inputScaleSqDiv16 = 0.0625f * inputScale * inputScale;

    for (int channel = 0; channel < channelCount; channel++) {
        float* src = input[channel];
        float* dst = output[channel];
        if (enabled && !enabled[channel]) {
            if (src != dst) memcpy(dst, src, sizeof(input[channel]));
            continue;
        }

        const sCompressor* comp = &settings[channel];

#if defined(__ADSP21000__) && (USE_SHARC_ASM_DYNAMICS == 1)
        sCompAsmParams cParams;
        cParams.thresholdDb       = comp->value_thresholdDb;
        cParams.slope             = comp->value_1_minus_1_by_ratio;
        cParams.makeup            = comp->value_makeup;
        cParams.coeffAttack       = comp->value_coeff_attack;
        cParams.holdTicks         = comp->value_hold_ticks;
        cParams.coeffRelease      = comp->value_coeff_release;
        cParams.kneeHalf          = comp->value_kneeHalf;
        cParams.kneeCoeff         = comp->value_kneeCoeff;
        cParams.env               = states[channel].envelope;
        cParams.holdTimer         = states[channel].holdTimer;
        cParams.inputScale        = inputScale;
        cParams.inputScaleSqDiv16 = inputScaleSqDiv16;
        fxCompressorBlock_asm(src, dst, &cParams);
        states[channel].envelope  = cParams.env;
        states[channel].holdTimer = cParams.holdTimer;
#else
        float makeUp = comp->value_makeup;
        float env = states[channel].envelope;
        int holdTimer = states[channel].holdTimer;
        float coeff = 0.0f;
        float normPower = 0.0f;

        if (comp->value_coeff_attack >= 0.999f) {
            /* 0ms attack: instant block-peak lookahead (zero transient overshoot) */
            float maxAbs = 0.0f;
            #pragma loop_count(OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES, OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES, OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES)
            #pragma vector_for
            for (int sample = 0; sample < OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES; sample++) {
                float a = fabsf(src[sample]);
                if (a > maxAbs) maxAbs = a;
            }
            float peakScaled = maxAbs * inputScale;
            normPower = peakScaled * peakScaled;
        } else {
            /* >0ms attack: SIMD sum-of-squares RMS without sqrtf() */
            float sumOfSquares = 0.0f;
            #pragma loop_count(OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES, OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES, OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES)
            #pragma vector_for
            for (int sample = 0; sample < OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES; sample++) {
                sumOfSquares += src[sample] * src[sample];
            }
            normPower = sumOfSquares * inputScaleSqDiv16;
        }

        /* Fast power-to-dBFS + 6-stage quadratic soft-knee gain computer */
        float levelDb = normPowerToDb_fast(normPower);
        float overshoot = levelDb - comp->value_thresholdDb;
        float kneeHalf = comp->value_kneeHalf;
        float targetGainDb = 0.0f;

        if (overshoot >= kneeHalf) {
            if (overshoot > 0.0f) {
                targetGainDb = -overshoot * comp->value_1_minus_1_by_ratio;
            }
        } else if (overshoot > -kneeHalf) {
            float u = overshoot + kneeHalf;
            targetGainDb = comp->value_kneeCoeff * (u * u);
        }

        float targetGainLinear = (targetGainDb < 0.0f) ? dbToLinear_fast(targetGainDb) : 1.0f;

        if (targetGainLinear < env) {
            coeff = comp->value_coeff_attack;
            holdTimer = (int)comp->value_hold_ticks;
        } else {
            if (holdTimer > 0) {
                holdTimer--;
            } else {
                coeff = comp->value_coeff_release;
            }
        }
        states[channel].holdTimer = holdTimer;

        /* 1-MAC envelope smoother in registers */
        float a1 = 1.0f - coeff;
        float b0 = coeff * targetGainLinear;

        #pragma loop_count(OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES, OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES, OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES)
        for (int sample = 0; sample < OPENAUDIODSP_COMPRESSOR_BLOCK_SAMPLES; sample++) {
            env = (env * a1) + b0;
            dst[sample] = (src[sample] * makeUp) * env;
        }
        states[channel].envelope = env;
#endif
    }
}
