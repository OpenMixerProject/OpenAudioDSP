/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WING_RUNTIME_H
#define WING_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { WING_REQUEST = 40, WING_REPLY = 24, WING_ARMED = 1, WING_FAULT = 2 };

/* WDSP v1: input is the payload after the FPGA's SS0 selector byte.
 * A bad frame produces no reply and never reaches the audio code. */
int wingRuntimeRequest(const unsigned char req[WING_REQUEST], unsigned char reply[WING_REPLY], unsigned flags);

void wingRuntimeSpiInit(void);
void wingRuntimeSpiPoll(unsigned flags);

#ifdef __cplusplus
}
#endif
#endif
