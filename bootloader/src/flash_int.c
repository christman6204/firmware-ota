#include "flash_int.h"
#include "stm32f10x_flash.h"

void flash_int_erase_page(uint32_t addr) {
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ErasePage(addr);
    FLASH_Lock();
}

void flash_int_program_halfword(uint32_t addr, uint16_t data) {
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ProgramHalfWord(addr, data);
    FLASH_Lock();
}

void flash_int_erase_area(uint32_t start, uint32_t size_kb) {
    uint32_t end = start + (size_kb * 1024u);
    for (uint32_t addr = start; addr < end; addr += 2048u) {
        flash_int_erase_page(addr);
    }
}
