#include <stdint.h>
#include <stddef.h>

static uint16_t crc_table[256];
static int crc_table_ready = 0;

static void crc16_init_table(void) {
    const uint16_t poly = 0xA001u;
    for (uint16_t i = 0; i < 256; i++) {
        uint16_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? (crc >> 1) ^ poly : (crc >> 1);
        }
        crc_table[i] = crc;
    }
    crc_table_ready = 1;
}

uint16_t crc16(const void *data, size_t len) {
    if (!crc_table_ready) crc16_init_table();

    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc_table[(crc ^ p[i]) & 0xFF];
    }
    return crc;
}
