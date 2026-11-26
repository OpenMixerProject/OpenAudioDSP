#ifndef OPEN_AUDIO_DSP_EFFECTS_AUDIO_IO_H
#define OPEN_AUDIO_DSP_EFFECTS_AUDIO_IO_H

#include "dsp2.h"

/* Inter-DSP TDM words carry float bits, not normalized PCM. */
extern int pm audioRxBuf[TDM_INPUTS * BUFFER_COUNT * BUFFER_SIZE];
extern int pm audioTxBuf[TDM_INPUTS * BUFFER_COUNT * BUFFER_SIZE];
extern int pm audioLinuxRxBuf[BUFFER_COUNT * BUFFER_SIZE_I2S];
extern int pm audioLinuxTxBuf[BUFFER_COUNT * BUFFER_SIZE_I2S];
extern int audioBufferOffset;

#ifdef __cplusplus
extern "C" {
#endif

void audioIoInit(void);
/* Reset any target-specific side-channel cursors when a block is received. */
void audioIoBeginBlock(void);

#ifdef __cplusplus
}
#endif

#endif
