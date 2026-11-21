/* SPDX-License-Identifier: GPL-3.0-only */
#include "runtime.h"
#include "version.h"

static uint32_t readWord(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void writeWord(unsigned char *p, uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = (value >> (24 - 8 * i)) & 255u;
}

static uint32_t crc32(const unsigned char *p, int n) {
    uint32_t crc = 0xffffffffu;
    for (int i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0);
    }
    return ~crc;
}

int wingRuntimeRequest(const unsigned char req[WING_REQUEST], unsigned char reply[WING_REPLY], unsigned flags) {
    const uint32_t check = readWord(req + 36);
    if (readWord(req) != 0x57445350u || crc32(req, 36) != check || req[11] != WING_DSP) return 0;

    unsigned status = 1; // Unsupported operation/class/index
    if (req[8] == 0 && req[9] == 0 && req[10] == 0) {
        status = 0; // Identify, including not-armed/faulted state
    }
    writeWord(reply, 0x57445352u); // WDSR
    writeWord(reply + 4, WING_ABI);
    writeWord(reply + 8, readWord(req + 4));
    writeWord(reply + 12, WING_DSP << 24 | status << 16 | flags << 8);
    writeWord(reply + 16, check);
    writeWord(reply + 20, crc32(reply, 20));
    return 1;
}
