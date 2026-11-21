/* SPDX-License-Identifier: GPL-3.0-only */
#include <processor_include.h>
#include <sru.h>
#include <stdint.h>
#include <string.h>

#include "audio.h"
#include "runtime.h"

#define RING 3
#define CP_ADDRESS 0x7ffffu

/* Each SPORT DMA engine fetches the four words backwards, starting at II. */
typedef struct {
    uint32_t next, count, modify, index;
} Tcb;

static WingRxBlock rx[RING];
static WingTxBlock tx[RING], render;
static Tcb rxTcb[RING][WING_RX_LANES], txTcb[RING][WING_TX_LANES];
volatile uint32_t wingSportArmed, wingAudioBlocks, wingAudioFault;

#define CLEAR_PIN(n) SRU(LOW, PBEN##n##_I)
#define CLOCK_ROUTE(s, clk, fs) \
    SRU(DAI_PB##clk##_O, SPORT##s##_CLK_I); SRU(DAI_PB##fs##_O, SPORT##s##_FS_I);
#define CLOCKS(s, clk, fs) CLOCK_ROUTE(s, clk, fs)
#define RX(s, ab, p) SRU(DAI_PB##p##_O, SPORT##s##_D##ab##_I);
#define TX(s, ab, p) SRU(SPORT##s##_D##ab##_O, DAI_PB##p##_I);

static volatile unsigned int *const ctl[] = {
    pSPCTL0, pSPCTL1, pSPCTL2, pSPCTL3, pSPCTL4, pSPCTL5, pSPCTL6, pSPCTL7
};
static volatile unsigned int *const mctl[] = {
    pSPMCTL0, pSPMCTL1, pSPMCTL2, pSPMCTL3, pSPMCTL4, pSPMCTL5, pSPMCTL6, pSPMCTL7
};
static volatile unsigned int *const select[] = {
    pMT0CS0, pMR1CS0, pMT2CS0, pMR3CS0, pMT4CS0, pMR5CS0, pMT6CS0, pMR7CS0
};
static volatile unsigned int *const compand[] = {
    pMT0CCS0, pMR1CCS0, pMT2CCS0, pMR3CCS0, pMT4CCS0, pMR5CCS0, pMT6CCS0, pMR7CCS0
};
#define CP_SINGLE(s,a) pCPSP##s##A,
#define CP_DUAL(s,a,b) CP_SINGLE(s,a) pCPSP##s##B,
static volatile unsigned int *const rxCp[] = { WING_RX_PORTS(CP_DUAL, CP_SINGLE) };
static volatile unsigned int *const txCp[] = { WING_TX_PORTS(CP_DUAL, CP_SINGLE) };

static uint32_t chain(const Tcb *tcb) {
    return (uint32_t)&tcb->index & CP_ADDRESS;
}

static void makeRing(int lanes, Tcb tcb[RING][lanes], int32_t buffers[RING][lanes][WING_LANE_WORDS],
                     volatile unsigned int *const cp[]) {
    for (int block = 0; block < RING; ++block) {
        int next = (block + 1) % RING;
        for (int lane = 0; lane < lanes; ++lane) {
            tcb[block][lane].next = chain(&tcb[next][lane]);
            tcb[block][lane].count = WING_LANE_WORDS;
            tcb[block][lane].modify = 1;
            tcb[block][lane].index = (uint32_t)&buffers[block][lane][0];
        }
    }
    for (int lane = 0; lane < lanes; ++lane) *cp[lane] = chain(&tcb[0][lane]);
}

static void configureSport(int sport, int dual, unsigned direction) {
    // MT/MR names are indexed by port, not by the programmed direction.
    *select[sport] = (1u << WING_LANE_SLOTS) - 1;
    *compand[sport] = 0;
    *mctl[sport] = NCH15 | MFD1 | MCEA | (dual ? MCEB : 0);
    // Stock edge/delay with reduced slot count: external clocks/FS, CKRE clear, no SPEN bits.
    *ctl[sport] = direction | SLEN32 | FSR | SDEN_A | SCHEN_A | (dual ? SDEN_B | SCHEN_B : 0);
}

static void enableTx(int enable) {
#define ENABLE_SINGLE(s,a) if (enable) { SRU(HIGH, PBEN##a##_I); } else { SRU(LOW, PBEN##a##_I); }
#define ENABLE_DUAL(s,a,b) ENABLE_SINGLE(s,a) ENABLE_SINGLE(s,b)
    WING_TX_PORTS(ENABLE_DUAL, ENABLE_SINGLE)
#undef ENABLE_SINGLE
#undef ENABLE_DUAL
}

static void armSport(void) {
    for (int s = 0; s < 8; ++s) {
        *ctl[s] = 0;
        *mctl[s] = 0;
    }
    CLEAR_PIN(01); CLEAR_PIN(02); CLEAR_PIN(03); CLEAR_PIN(04); CLEAR_PIN(05);
    CLEAR_PIN(06); CLEAR_PIN(07); CLEAR_PIN(08); CLEAR_PIN(09); CLEAR_PIN(10);
    CLEAR_PIN(11); CLEAR_PIN(12); CLEAR_PIN(13); CLEAR_PIN(14); CLEAR_PIN(15);
    CLEAR_PIN(16); CLEAR_PIN(17); CLEAR_PIN(18); CLEAR_PIN(19); CLEAR_PIN(20);
    makeRing(WING_RX_LANES, rxTcb, rx, rxCp);
    makeRing(WING_TX_LANES, txTcb, tx, txCp);

#define ARM_RX_SINGLE(s,a) CLOCKS(s, WING_RX_CLOCK, WING_RX_FS) RX(s,A,a) configureSport(s, 0, 0);
#define ARM_RX_DUAL(s,a,b) CLOCKS(s, WING_RX_CLOCK, WING_RX_FS) RX(s,A,a) RX(s,B,b) configureSport(s, 1, 0);
#define ARM_TX_SINGLE(s,a) CLOCKS(s, WING_TX_CLOCK, WING_TX_FS) TX(s,A,a) configureSport(s, 0, SPTRAN);
#define ARM_TX_DUAL(s,a,b) CLOCKS(s, WING_TX_CLOCK, WING_TX_FS) TX(s,A,a) TX(s,B,b) configureSport(s, 1, SPTRAN);
    WING_RX_PORTS(ARM_RX_DUAL, ARM_RX_SINGLE)
    WING_TX_PORTS(ARM_TX_DUAL, ARM_TX_SINGLE)
    enableTx(1);
    wingSportArmed = 1;
}

/* While DMA operates on block b it has already fetched b's TCB and CPSP
 * holds the address of b+1. Wait for every lane, not just a master lane.
 * FPGA must keep FS idle until all SPORTs are armed and hold it continuous
 * thereafter. No claim of electrical/frame sync is made by this poll. */
static int allAt(int nextTcb) {
    for (int lane = 0; lane < WING_RX_LANES; ++lane)
        if ((*rxCp[lane] & CP_ADDRESS) != chain(&rxTcb[nextTcb][lane])) return 0;
    for (int lane = 0; lane < WING_TX_LANES; ++lane)
        if ((*txCp[lane] & CP_ADDRESS) != chain(&txTcb[nextTcb][lane])) return 0;
    return 1;
}

int main(void) {
    armSport();
    wingRuntimeSpiInit();
    for (int block = 0; ; block = (block + 1) % RING) {
        int nextTcb = (block + 2) % RING;
        while (!allAt(nextTcb)) wingRuntimeSpiPoll(WING_ARMED);
        wingAudioProcess((const int32_t (*)[WING_LANE_WORDS])rx[block], render);
        if (!allAt(nextTcb)) {
            enableTx(0);
            wingAudioFault = 1;
            for (;;) wingRuntimeSpiPoll(WING_FAULT); // report fault until external reset
        }
        memcpy(tx[nextTcb], render, sizeof(render));
        ++wingAudioBlocks;
    }
}
