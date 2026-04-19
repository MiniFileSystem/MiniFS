/*
 * crc32c.c - Portable software CRC-32C (Castagnoli) implementation.
 * Uses a 256-entry table built on first use.
 * Polynomial: 0x1EDC6F41 (reflected 0x82F63B78).
 */
#include "crc32c.h"

#include <stdbool.h>

static uint32_t g_table[256];
static bool     g_table_ready = false;

static void crc32c_init_table(void)
{
    const uint32_t poly = 0x82F63B78U;  /* reflected Castagnoli */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
        }
        g_table[i] = c;
    }
    g_table_ready = true;
}

uint32_t crc32c_update(uint32_t crc, const void *buf, size_t len)
{
    if (!g_table_ready) {
        crc32c_init_table();
    }
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    while (len--) {
        crc = g_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t crc32c(const void *buf, size_t len)
{
    return crc32c_update(0, buf, len);
}
