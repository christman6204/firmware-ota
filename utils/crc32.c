#include <stdint.h>
#include <stddef.h>

static uint32_t crc_table[256];
static int crc_table_ready = 0;

static void crc32_init_table(void) {
    const uint32_t poly = 0xEDB88320u;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? (crc >> 1) ^ poly : (crc >> 1);
        }
        crc_table[i] = crc;
    }
    crc_table_ready = 1;
}

uint32_t crc32(const void *data, size_t len) {
    if (!crc_table_ready) crc32_init_table();

    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc_table[(crc ^ p[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFu;
}
