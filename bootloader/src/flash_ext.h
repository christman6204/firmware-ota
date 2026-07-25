#ifndef FLASH_EXT_H
#define FLASH_EXT_H
#include <stdint.h>

#define FLASH_EXT_FW_HEADER_ADDR   0x00000000u
#define FLASH_EXT_FW_AREA_ADDR     0x00001000u
#define FLASH_EXT_BACKUP_HEADER    0x00081000u
#define FLASH_EXT_BACKUP_AREA      0x00082000u
#define FLASH_EXT_SECTOR_SIZE      4096u

#define FW_HEADER_MAGIC            0x46574844u
#define FW_BACKUP_MAGIC            0x424B5550u

typedef struct {
    uint32_t magic;
    uint32_t size;
    char     version[16];
    uint32_t receive_offset;
} fw_header_t;

void      flash_ext_init(void);
void      flash_ext_read(uint32_t addr, uint8_t *buf, uint32_t len);
void      flash_ext_write_page(uint32_t addr, const uint8_t *buf, uint32_t len);
void      flash_ext_erase_sector(uint32_t addr);
void      flash_ext_erase_4k(uint32_t addr);
uint32_t  flash_ext_read_id(void);
#endif
