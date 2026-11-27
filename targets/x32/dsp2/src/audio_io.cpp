/* X32 ADSP-21371 SPORT DMA and S/PDIF transport. */
#include "audio.h"
#include "system.h"

#pragma file_attr("prefersMem=internal")

static volatile int spdifSamplePointer = 0;
static volatile bool spdifLeftChannel = true;
int audioBufferOffset = 0;
int pm audioRxBuf[TDM_INPUTS * BUFFER_COUNT * BUFFER_SIZE] = {0};
int pm audioTxBuf[TDM_INPUTS * BUFFER_COUNT * BUFFER_SIZE] = {0};
int pm audioLinuxRxBuf[BUFFER_COUNT * BUFFER_SIZE_I2S] = {0};
int pm audioLinuxTxBuf[BUFFER_COUNT * BUFFER_SIZE_I2S] = {0};
int pm audioRx_tcb[5][BUFFER_COUNT][4];
int pm audioTx_tcb[5][BUFFER_COUNT][4];

void audioIoInit(void) {
	for (int i_buf = 0; i_buf < BUFFER_COUNT; i_buf++) {
		int nextBuffer = (i_buf + 1) % BUFFER_COUNT;

		// setup TCB for TDM Rx/Tx
		for (int i_tcb = 0; i_tcb < TDM_INPUTS; i_tcb++) {
			audioRx_tcb[i_tcb][i_buf][0] = ((int)&audioRx_tcb[i_tcb][nextBuffer][0] + 3);
			audioRx_tcb[i_tcb][i_buf][1] = BUFFER_SIZE;
			audioRx_tcb[i_tcb][i_buf][2] = 1;
			audioRx_tcb[i_tcb][i_buf][3] = (int)&audioRxBuf[(BUFFER_COUNT * BUFFER_SIZE * i_tcb) + (BUFFER_SIZE * i_buf)];
			
			audioTx_tcb[i_tcb][i_buf][0] = ((int)&audioTx_tcb[i_tcb][nextBuffer][0] + 3);
			audioTx_tcb[i_tcb][i_buf][1] = BUFFER_SIZE;
			audioTx_tcb[i_tcb][i_buf][2] = 1;
			audioTx_tcb[i_tcb][i_buf][3] = (int)&audioTxBuf[(BUFFER_COUNT * BUFFER_SIZE * i_tcb) + (BUFFER_SIZE * i_buf)];
		}

		// setup TCB for I2S Rx/Tx (last element in TCB-array, so we use "TDM_INPUTS" as index)
		audioRx_tcb[TDM_INPUTS][i_buf][0] = ((int)&audioRx_tcb[TDM_INPUTS][nextBuffer][0] + 3);
		audioRx_tcb[TDM_INPUTS][i_buf][1] = BUFFER_SIZE_I2S;
		audioRx_tcb[TDM_INPUTS][i_buf][2] = 1;
		audioRx_tcb[TDM_INPUTS][i_buf][3] = (int)&audioLinuxRxBuf[BUFFER_SIZE_I2S * i_buf];
		
		audioTx_tcb[TDM_INPUTS][i_buf][0] = ((int)&audioTx_tcb[TDM_INPUTS][nextBuffer][0] + 3);
		audioTx_tcb[TDM_INPUTS][i_buf][1] = BUFFER_SIZE_I2S;
		audioTx_tcb[TDM_INPUTS][i_buf][2] = 1;
		audioTx_tcb[TDM_INPUTS][i_buf][3] = (int)&audioLinuxTxBuf[BUFFER_SIZE_I2S * i_buf];
	}
}

void audioIoBeginBlock(void) {
	spdifSamplePointer = 0;
	spdifLeftChannel = true;
}

void audioRxISR(uint32_t iid, void *handlerarg) {
	if (audioProcessing) {
		audioGlitchCounter++;
		systemCrash();
	}
	audioReady = 1;
}

void audioSpdifTxXlrISR(uint32_t iid, void* handlerarg) {
	if (spdifLeftChannel) {
		*pTXSP6A = ((int32_t)audioBuffer[TAP_INPUT][DSP_BUF_IDX_SPDIF_LEFT][spdifSamplePointer]) & 0xFFFFFF00;
		spdifLeftChannel = false;
	}else{
		*pTXSP6A = ((int32_t)audioBuffer[TAP_INPUT][DSP_BUF_IDX_SPDIF_RIGHT][spdifSamplePointer]) & 0xFFFFFF00;
		spdifLeftChannel = true;
		spdifSamplePointer++;
	}
}
