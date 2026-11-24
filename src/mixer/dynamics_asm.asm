/* SPDX-License-Identifier: GPL-3.0-only
 * OpenAudioDSP - ADSP-21371 SHARC Assembly Dynamics Kernels (CCES easm21k)
 * Copyright 2025-2026 OpenMixerProject
 *
 * Scale-agnostic 16-sample compressor kernel optimized for ADSP-21371 SHARC:
 * - Zero-overhead hardware loops (LCNTR=16)
 * - 0ms attack instant block-peak lookahead (ABS + MAX) with zero transient overshoot
 * - Hardware LOGB / SCALB fast power-to-dBFS and dB-to-linear (< 0.03 dB error, zero sqrtf/log/pow)
 * - 6-stage quadratic soft-knee gain computer (0 dB hard .. 36 dB soft 5)
 * - 40-bit extended-precision ALU accumulation throughout
 */

#include <asm_sprt.h>

// Struct field offsets (must match dynamics_asm.h)
#define COMP_OFF_THRESH_DB     0
#define COMP_OFF_SLOPE         1
#define COMP_OFF_MAKEUP        2
#define COMP_OFF_ATTACK        3
#define COMP_OFF_HOLD_TICKS    4
#define COMP_OFF_RELEASE       5
#define COMP_OFF_KNEE_HALF     6
#define COMP_OFF_KNEE_COEFF    7
#define COMP_OFF_ENV           8
#define COMP_OFF_HOLD_TIMER    9
#define COMP_OFF_INPUT_SCALE   10
#define COMP_OFF_SCALE_SQ_D16  11

#define SAMPLES_PER_BLOCK      16

.SECTION/DM seg_dmda;
.VAR _dyn_c_norm_floor    = 1.0e-14;        // -140 dBFS normalized power floor
.VAR _dyn_c_neg140        = -140.0;
.VAR _dyn_c_pos24         = 24.0;
.VAR _dyn_c_0p999         = 0.999;
.VAR _dyn_c_1p0           = 1.0;
.VAR _dyn_c_log2_a        = 1.6797000;      // quadratic log2(m) coeff a
.VAR _dyn_c_log2_b        = 0.3398500;      // quadratic log2(m) coeff b
.VAR _dyn_c_10log10_2     = 3.01029995664;  // 10 * log10(2)
.VAR _dyn_c_inv_20log10_2 = 0.16609640474;  // 1 / (20 * log10(2))
.VAR _dyn_c_exp2_c1       = 0.65685425;     // quadratic 2^xf coeff c1
.VAR _dyn_c_exp2_c2       = 0.34314575;     // quadratic 2^xf coeff c2

.SECTION/PM seg_pmco;

// ============================================================================
// Internal helper: _asm_normPowerToDb_fast
//   Input : F2 = normalized power = mean(x^2) * inputScale^2 (or (peak * inputScale)^2)
//   Output: F0 = levelDb in dBFS (clamped at -140.0 dBFS)
//   Clobbers: R0, R1, F0, F1, F2, F4
// ============================================================================
_asm_normPowerToDb_fast:
	F4 = DM(_dyn_c_norm_floor);
	COMP(F2, F4);
	IF LE JUMP _p2db_floor;

	// Hardware LOGB extracts unbiased IEEE-754 exponent in 1 cycle
	R0 = LOGB F2;
	R1 = -R0;
	// Hardware SCALB shifts mantissa m into [1.0, 2.0) in 1 cycle
	F0 = SCALB F2 BY R1;
	F1 = DM(_dyn_c_1p0);
	F2 = F0 - F1;                       // F2 = m - 1.0
	F4 = DM(_dyn_c_log2_b);
	F4 = F4 * F0;                       // 0.33985 * m
	F1 = DM(_dyn_c_log2_a);
	F1 = F1 - F4;                       // 1.6797 - 0.33985 * m
	F2 = F2 * F1;                       // log2_m
	F1 = FLOAT R0;                      // (float)exp
	F1 = F1 + F2;                       // exp + log2_m
	F4 = DM(_dyn_c_10log10_2);
	F0 = F1 * F4;                       // 3.0103 * log2(normPower)
	RTS;

_p2db_floor:
	F0 = DM(_dyn_c_neg140);
	RTS;

// ============================================================================
// Internal helper: _asm_dbToLinear_fast
//   Input : F0 = dB
//   Output: F0 = linear gain = 10^(dB/20)
//   Clobbers: R0, R1, F0, F1, F2, F4
// ============================================================================
_asm_dbToLinear_fast:
	F4 = DM(_dyn_c_neg140);
	COMP(F0, F4);
	IF LE JUMP _db2lin_zero;

	F4 = DM(_dyn_c_pos24);
	F0 = MIN(F0, F4);

	F4 = DM(_dyn_c_inv_20log10_2);
	F1 = F0 * F4;                       // x = dB / (20 * log10(2))
	R0 = TRUNC F1;                      // integer part (towards zero)
	F2 = FLOAT R0;
	COMP(F2, F1);
	IF GT R0 = R0 - 1;                  // floor(x) for negative values
	F2 = FLOAT R0;
	F2 = F1 - F2;                       // xf in [0.0, 1.0)

	// mant = 1.0 + xf * (0.65685425 + 0.34314575 * xf)
	F4 = DM(_dyn_c_exp2_c2);
	F4 = F4 * F2;
	F1 = DM(_dyn_c_exp2_c1);
	F1 = F1 + F4;
	F1 = F1 * F2;
	F4 = DM(_dyn_c_1p0);
	F1 = F1 + F4;

	// Hardware SCALB multiplies by 2^R0 in 1 cycle
	F0 = SCALB F1 BY R0;
	RTS;

_db2lin_zero:
	F0 = F0 - F0;                       // 0.0f
	RTS;

// ============================================================================
// void fxCompressorBlock_asm(const float* src, float* dst, sCompAsmParams* params)
//   R4  = src
//   R8  = dst
//   R12 = params
// ============================================================================
.GLOBAL _fxCompressorBlock_asm;
_fxCompressorBlock_asm:
	leaf_entry;

	DM(I7, M7) = R3;
	DM(I7, M7) = R5;
	DM(I7, M7) = R6;
	DM(I7, M7) = R7;
	DM(I7, M7) = R9;
	DM(I7, M7) = R10;
	DM(I7, M7) = R11;
	DM(I7, M7) = I2;
	DM(I7, M7) = I3;

	I2  = R4;                           // src base
	I3  = R12;                          // params base
	I12 = R8;                           // dst base

	// Check 0ms attack (coeffAttack >= 0.999f)
	F0 = DM(COMP_OFF_ATTACK, I3);
	F1 = DM(_dyn_c_0p999);
	COMP(F0, F1);
	I4 = I2;
	F2 = F2 - F2;
	IF LT JUMP _comp_rms_detect;

	// --- 0ms Attack: Instant Block-Peak Lookahead (ABS + MAX) ---
	LCNTR = SAMPLES_PER_BLOCK, DO _comp_peak_loop UNTIL LCE;
		F0 = DM(I4, M6);
		F0 = ABS F0;
_comp_peak_loop:
		F2 = MAX(F0, F2);

	F4 = DM(COMP_OFF_INPUT_SCALE, I3);
	F2 = F2 * F4;                       // peak * inputScale
	F2 = F2 * F2;                       // normalized peak power
	JUMP _comp_gain_calc;

_comp_rms_detect:
	// --- >0ms Attack: Zero-overhead 16-sample Sum-of-Squares ---
	LCNTR = SAMPLES_PER_BLOCK, DO _comp_rms_loop UNTIL LCE;
		F0 = DM(I4, M6);
		F1 = F0 * F0;
_comp_rms_loop:
		F2 = F2 + F1;

	F4 = DM(COMP_OFF_SCALE_SQ_D16, I3);
	F2 = F2 * F4;                       // normalized mean square power

_comp_gain_calc:
	CALL _asm_normPowerToDb_fast;       // F0 = levelDb

	F1 = DM(COMP_OFF_THRESH_DB, I3);
	F5 = F0 - F1;                       // F5 = overshoot
	F6 = DM(COMP_OFF_KNEE_HALF, I3);    // F6 = kneeHalf
	F0 = F0 - F0;                       // F0 = targetGainDb = 0.0f

	// if (overshoot >= kneeHalf) -> above knee (also handles hard knee)
	COMP(F5, F6);
	IF LT JUMP _comp_check_knee;
	F1 = F1 - F1;
	COMP(F5, F1);                       // overshoot > 0.0f?
	IF LE JUMP _comp_to_lin;
	F4 = DM(COMP_OFF_SLOPE, I3);
	F0 = F5 * F4;
	F0 = -F0;                           // targetGainDb = -overshoot * slope
	JUMP _comp_to_lin;

_comp_check_knee:
	F7 = -F6;                           // -kneeHalf
	COMP(F5, F7);
	IF LE JUMP _comp_to_lin;            // below knee -> targetGainDb = 0.0f
	// Inside quadratic soft-knee: u = overshoot + kneeHalf; targetGainDb = kneeCoeff * u * u
	F1 = F5 + F6;
	F1 = F1 * F1;
	F4 = DM(COMP_OFF_KNEE_COEFF, I3);
	F0 = F1 * F4;

_comp_to_lin:
	F1 = F1 - F1;
	COMP(F0, F1);
	IF GE JUMP _comp_unity_target;
	CALL _asm_dbToLinear_fast;
	F5 = F0;
	JUMP _comp_ahr;

_comp_unity_target:
	F5 = DM(_dyn_c_1p0);

_comp_ahr:
	F6 = DM(COMP_OFF_ENV, I3);          // F6 = env
	R7 = DM(COMP_OFF_HOLD_TIMER, I3);   // R7 = holdTimer
	F9 = F9 - F9;                       // F9 = coeff = 0.0f

	COMP(F5, F6);                       // targetGainLinear < env?
	IF GE JUMP _comp_hold_rel;
	// Attack
	F9 = DM(COMP_OFF_ATTACK, I3);
	F1 = DM(COMP_OFF_HOLD_TICKS, I3);
	R7 = TRUNC F1;
	JUMP _comp_apply_setup;

_comp_hold_rel:
	R7 = PASS R7;
	IF LE JUMP _comp_release;
	R7 = R7 - 1;
	JUMP _comp_apply_setup;

_comp_release:
	F9 = DM(COMP_OFF_RELEASE, I3);

_comp_apply_setup:
	DM(COMP_OFF_HOLD_TIMER, I3) = R7;

	F1  = DM(_dyn_c_1p0);
	F10 = F1 - F9;                      // a1 = 1.0 - coeff
	F11 = F9 * F5;                      // b0 = coeff * targetGainLinear
	F3  = DM(COMP_OFF_MAKEUP, I3);

	I4 = I2;
	LCNTR = SAMPLES_PER_BLOCK, DO _comp_apply_loop UNTIL LCE;
		F6 = F6 * F10;
		F0 = DM(I4, M6), F6 = F6 + F11; // parallel load src[s] + env = env*a1 + b0
		F0 = F0 * F3;                   // src[s] * makeup
		F0 = F0 * F6;                   // * env
_comp_apply_loop:
		DM(I12, M6) = F0;

	DM(COMP_OFF_ENV, I3) = F6;

	MODIFY(I7, 1);
	I3  = DM(I7, M6);
	I2  = DM(I7, M6);
	R11 = DM(I7, M6);
	R10 = DM(I7, M6);
	R9  = DM(I7, M6);
	R7  = DM(I7, M6);
	R6  = DM(I7, M6);
	R5  = DM(I7, M6);
	R3  = DM(I7, M5);

	leaf_exit;
_fxCompressorBlock_asm.end:
