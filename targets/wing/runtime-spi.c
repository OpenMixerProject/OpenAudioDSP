/* SPDX-License-Identifier: GPL-3.0-only */
#include <processor_include.h>
#include <sru.h>
#include "runtime.h"

static unsigned char request[WING_REQUEST], reply[WING_REPLY];
static unsigned received, sent;
static int deselected, replying;

#define SLAVE_SPI (ISSEN | MSBF | SPIEN) /* 8-bit, CPOL=0, CPHA=0, core mode */

void wingRuntimeSpiInit(void) {
    SRU(LOW, DPI_PBEN01_I);
    SRU(LOW, DPI_PBEN02_I);
    SRU(LOW, DPI_PBEN03_I);
    SRU(LOW, DPI_PBEN04_I);
    SRU(DPI_PB01_O, SPI_MOSI_I);
    SRU(DPI_PB03_O, SPI_CLK_I);
    SRU(DPI_PB04_O, SPI_DS_I);
    SRU(SPI_MISO_O, DPI_PB02_I);
    *pSPICTL = 0;
    *pSPIDMAC = 0;
    *pSPIFLG = DS0EN;
}

/* Single writer on the audio thread, never called inside an audio block.
 * Require deselect after boot, and reset the byte state at each deselect. */
void wingRuntimeSpiPoll(unsigned flags) {
    if (*pDPI_PIN_STAT & (1u << 3)) {
        if (!deselected) {
            *pSPICTL = 0;
            if (*pSPISTAT & RXS) (void)*pRXSPI;
            *pSPICTL = SLAVE_SPI;
            *pTXSPI = 255;
            SRU(SPI_MISO_PBEN_O, DPI_PBEN02_I);
            received = sent = 0;
            replying = 0;
            deselected = 1;
        }
        return;
    }
    if (!(*pSPICTL & SPIEN)) return; // still in the boot select
    deselected = 0;
    if (*pSPISTAT & RXS) {
        const unsigned data = *pRXSPI & 255u;
        if (received < WING_REQUEST) {
            request[received++] = data;
            if (received == WING_REQUEST) replying = wingRuntimeRequest(request, reply, flags);
        }
    }
    if (!(*pSPISTAT & TXS)) {
        *pTXSPI = replying && sent < WING_REPLY ? reply[sent++] : 255;
    }
}
