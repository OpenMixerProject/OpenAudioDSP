/* SPDX-License-Identifier: GPL-3.0-only */
#include "audio.h"
#include "runtime.h"
#include <string.h>

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

    wing_test_result[2] = WING_DSP;
    wing_test_result[3] = WING_SAMPLE_RATE;
    finish();
    return wing_test_result[1] != 0;
}
