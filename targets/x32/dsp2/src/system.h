#ifndef __SYSTEM_H__
#define __SYSTEM_H__

#include "dsp2.h"
#include "sharc_board.h"
#include "audio_io.h"

extern int pm audioTx_tcb[5][BUFFER_COUNT][4];
extern int pm audioRx_tcb[5][BUFFER_COUNT][4];

#ifdef __cplusplus
extern "C" {
#endif

// function prototypes
void systemPllInit(void);
void systemExternalMemoryInit(void);

void systemPcgInit(void);
void systemSruInit(void);
void systemSportInit(void);
void systemSpdifTxInit(void);

void systemCrash(void);

void audioRxISR(uint32_t iid, void *handlerarg);
void audioSpdifTxXlrISR(uint32_t iid, void *handlerarg);

#ifdef __cplusplus
}
#endif

#endif
