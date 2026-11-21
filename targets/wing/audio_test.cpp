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

#if WING_DSP == 4
static void command(unsigned op, unsigned pair, uint32_t gain, uint32_t q, uint32_t outputFilter,
                    uint32_t requestCrc, unsigned flags, unsigned status, uint32_t replyCrc) {
    unsigned char req[WING_REQUEST] = {0x57, 0x44, 0x53, 0x50, 0x18, 0x27, 0x46, 0x91};
    unsigned char reply[WING_REPLY];
    unsigned char expected[WING_REPLY] = {0x57, 0x44, 0x53, 0x52, 0, 0, 0, 1, 0x18, 0x27, 0x46, 0x91, 4};
    req[8] = op; req[9] = 5; req[10] = pair; req[11] = 4;
    be32(req + 12, gain);
    be32(req + 16, q);
    be32(req + 24, op == 2 ? 0x3f800000 : 0);
    be32(req + 28, op == 2 ? 0x3f800000 : 0);
    be32(req + 32, outputFilter);
    be32(req + 36, requestCrc);
    expected[13] = status; expected[14] = flags;
    be32(expected + 16, requestCrc);
    be32(expected + 20, replyCrc);
    check(wingRuntimeRequest(req, reply, flags) && memcmp(reply, expected, sizeof(reply)) == 0);
}

static void overdrive() {
    const uint32_t one = 0x3f800000, two = 0x40000000, four = 0x40800000, sixteen = 0x41800000;
    for (int n = 0; n < 16; ++n) {
        word(rx, 0, n) = 0x04000000;  // left +0.25
        word(rx, 1, n) = -0x02000000; // right -0.125
    }
    command(2, 0, two, 0, one, 0x4b0a0883, 0, 2, 0xa71cae51);
    command(2, 0, two, 0, one, 0x4b0a0883, WING_FAULT, 2, 0xead40f5a);
    wingAudioProcess(rx, tx);
    check(word(tx, 0, 0) == word(rx, 0, 0) && word(tx, 1, 15) == word(rx, 1, 15));

    // At maximum Q, the reciprocal soft clip underflows cleanly from reset.
    command(2, 0, four, 0x3f7fffef, one, 0x63fdff86, WING_ARMED, 0, 0x05450dac);
    wingAudioProcess(rx, tx);
    check(word(tx, 0, 0) == 0 && word(tx, 1, 15) == 0);
    command(2, 0, two, 0, one, 0x4b0a0883, WING_ARMED, 0, 0xfbdf6cdd);
    wingAudioProcess(rx, tx);
    // At Q=0, soft clip is x/(1+|x|), followed by the DC blocker.
    check(word(tx, 0, 0) > 0x05400000 && word(tx, 0, 0) < 0x05600000);
    check(word(tx, 1, 0) < -0x03200000 && word(tx, 1, 0) > -0x03400000);

    // Reject the last operand even though the first operand is a valid new gain.
    command(2, 0, sixteen, 0, 0x7fc00000, 0x2a358a39, WING_ARMED, 3, 0x449be4d0);
    command(2, 1, sixteen, 0, one, 0x26a6f953, WING_ARMED, 1, 0xcff2ee9b);
    wingAudioProcess(rx, tx);
    check(word(tx, 0, 0) > 0x05100000 && word(tx, 0, 0) < 0x05400000);

    command(2, 0, four, 0, one, 0x409e3ae3, WING_ARMED, 0, 0x7606ced9);
    wingAudioProcess(rx, tx);
    check(word(tx, 0, 0) > 0x07c00000 && word(tx, 1, 0) < -0x05000000);
    for (int slot = 2; slot < 16; ++slot)
        for (int n = 0; n < 16; ++n) check(word(tx, slot, n) == word(rx, slot, n));
    for (int slot = 16; slot < 32; ++slot)
        for (int n = 0; n < 16; ++n) check(word(tx, slot, n) == 0);
    command(1, 0, 0, 0, 0, 0xe3d231af, WING_ARMED, 0, 0xccc6d8e6);
    wingAudioProcess(rx, tx);
    for (int slot = 0; slot < 2; ++slot)
        for (int n = 0; n < 16; ++n) check(word(tx, slot, n) == word(rx, slot, n));
}
#endif

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

#if WING_DSP == 4
    overdrive();
#endif
    wing_test_result[2] = WING_DSP;
    wing_test_result[3] = WING_SAMPLE_RATE;
    finish();
    return wing_test_result[1] != 0;
}
