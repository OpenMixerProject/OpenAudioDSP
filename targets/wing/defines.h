/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WING_EFFECTS_DEFINES_H
#define WING_EFFECTS_DEFINES_H

#include <math.h>
#include "profile.h"
#define SAMPLES_IN_BUFFER WING_BLOCK
#define SAMPLERATE_MAX WING_SAMPLE_RATE
#define INT32_TO_FLOAT_NORM (1.0f / 2147483648.0f)
#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795f
#endif
/* Only effects that don't dereference the old X32 memory slot may be linked. */
#define SDRAM_AUDIO_START 0
#define SDRAM_AUDIO_SIZE_BYTE 0

#endif
