/* SPDX-License-Identifier: GPL-3.0-only */
#include "audio.h"
#include "runtime.h"
#include <string.h>

#if WING_DSP != 4
#include "openaudiodsp/compressor.h"
#include "openaudiodsp/gate.h"
extern sGate wingGates[];
extern bool wingCompEnabled[], wingEqEnabled[];
extern sCompressor wingCompressor[];
extern float pm wingEqCoefficients[];
#endif

static WingRxBlock rx;
static WingTxBlock tx;
extern "C" {
volatile int wing_test_result[4]; // checks, failures, target, sample rate
void wing_test_done(void);
}

void wing_test_done(void) {
    wing_test_result[3] = WING_SAMPLE_RATE;
}

static void (*volatile finish)(void) = wing_test_done;

static void check(bool condition) {
    ++wing_test_result[0];
    if (!condition) ++wing_test_result[1];
}

static int32_t &word(int32_t block[][WING_LANE_WORDS], int slot, int frame) {
    // Use the agreed wire stride, independent of the audio implementation.
    return block[slot / 16][frame * 16 + slot % 16];
}

static void be32(unsigned char *dst, uint32_t value) {
    for (int i = 0; i < 4; ++i) dst[i] = (value >> (24 - 8 * i)) & 255;
}

static void identify() {
    // Independent CRC fixtures from zlib.crc32 over big-endian frames.
    const uint32_t requestCrc[] = {0xb39c6643, 0x8671d010, 0x23fa401e, 0xedaabcb6};
    const uint32_t replyCrc[] = {0x69691c2a, 0xc1d459d9, 0x0c52e30c, 0x9cbdd8fb};
    const int core = WING_DSP - 1;
    unsigned char req[WING_REQUEST] = {0x57, 0x44, 0x53, 0x50, 0x18, 0x27, 0x46, 0x91};
    unsigned char reply[WING_REPLY];
    unsigned char expected[WING_REPLY] = {
        0x57, 0x44, 0x53, 0x52, 0, 0, 0, 1, 0x18, 0x27, 0x46, 0x91, WING_DSP, 0, 1, 0
    };
    req[11] = WING_DSP % 4 + 1;
    be32(req + 36, requestCrc[req[11] - 1]);
    check(!wingRuntimeRequest(req, reply, WING_ARMED)); // valid CRC, wrong core
    req[11] = WING_DSP;
    be32(req + 36, requestCrc[core]);
    req[4] ^= 1;
    check(!wingRuntimeRequest(req, reply, WING_ARMED));
    req[4] ^= 1;
    be32(expected + 16, requestCrc[core]);
    be32(expected + 20, replyCrc[core]);
    check(wingRuntimeRequest(req, reply, WING_ARMED) && memcmp(reply, expected, sizeof(reply)) == 0);
    check(wingRuntimeRequest(req, reply, WING_FAULT) && reply[13] == 0 && reply[14] == WING_FAULT);
    check(wingRuntimeRequest(req, reply, 0) && reply[13] == 0 && reply[14] == 0);
}

int main(void) {
    const int used = WING_DSP == 4 ? 16 : WING_DSP == 3 ? 32 : WING_DSP == 2 ? 50 : 64;
    for (int slot = 0; slot < WING_RX_LANES * 16; ++slot)
        for (int n = 0; n < 16; ++n)
            word(rx, slot, n) = ((slot + 3) * 175493 + n * 3571) * (n % 2 ? -1 : 1);
    word(rx, 0, 0) = INT32_MAX;
    word(rx, used - 1, 15) = INT32_MIN;
    for (int slot = 0; slot < WING_TX_LANES * 16; ++slot)
        for (int n = 0; n < 16; ++n) word(tx, slot, n) = -18;

    identify();
    wingAudioProcess(rx, tx);
    for (int slot = 0; slot < WING_TX_LANES * 16; ++slot)
        for (int n = 0; n < 16; ++n)
            check(word(tx, slot, n) == (slot < used ? word(rx, slot, n) : 0));

#if WING_DSP != 4
    const int gate = WING_DSP == 1 ? 55 : WING_DSP == 2 ? 49 : 23;
    const int comp = 15;
    const int fxReturn = gate + 1;
    wingGates[gate].value_enable = true;
    wingGates[gate].value_threshold = 0.5f;
    wingGates[gate].value_coeff_attack = 1.0f;
    wingGates[gate].value_coeff_release = 1.0f;
    wingGates[gate].value_hold_ticks = 1;
    wingCompEnabled[comp] = true;
    wingCompressor[comp].value_makeup = 0.5f;
    wingCompressor[comp].value_coeff_release = 1.0f;
    for (int n = 0; n < 16; ++n) {
        word(rx, gate, n) = 0x02000000;
        word(rx, comp, n) = -0x0c000000;
    }
#if !defined(WING_TEST_HOST)
    // Adjacent to compressor, but across a lane boundary. Use real ADI EQ.
    const int eq = 16;
    wingEqEnabled[eq] = true;
    wingEqCoefficients[20 * eq] = 0.5f;
    wingEqCoefficients[20 * eq + 1] = 1.0f;
    wingEqCoefficients[20 * eq + 10] = 1.0f;
    wingEqCoefficients[20 * eq + 11] = 1.0f;
    for (int n = 0; n < 16; ++n) word(rx, eq, n) = (n - 5) * 0x01000000;
#endif
    if (WING_DSP != 2) {
        wingCompEnabled[fxReturn] = true;
        wingCompressor[fxReturn].value_makeup = 0.25f;
        wingCompressor[fxReturn].value_coeff_release = 1.0f;
        for (int n = 0; n < 16; ++n) word(rx, fxReturn, n) = 0x08000000;
    }
    wingAudioProcess(rx, tx);
    for (int n = 0; n < 16; ++n) {
        check(word(tx, gate, n) == 0);
        check(word(tx, comp, n) == -0x06000000);
        check(word(tx, gate - 1, n) == word(rx, gate - 1, n));
#if !defined(WING_TEST_HOST)
        check(word(tx, eq, n) == (n - 5) * 0x00800000);
#endif
        if (WING_DSP != 2) check(word(tx, fxReturn, n) == 0x02000000);
    }
    for (int n = 0; n < 16; ++n) word(rx, gate, n) = 0x10000000;
    wingAudioProcess(rx, tx);
    for (int n = 0; n < 16; ++n) word(rx, gate, n) = 0x02000000;
    wingAudioProcess(rx, tx);
    check(word(tx, gate, 15) == 0x02000000); // holds the previous block's open envelope
    wingAudioProcess(rx, tx);
    check(word(tx, gate, 0) == 0);
#endif

    wing_test_result[2] = WING_DSP;
    wing_test_result[3] = WING_SAMPLE_RATE;
    finish();
    return wing_test_result[1] != 0;
}
