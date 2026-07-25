#ifndef FLASH_INT_H
#define FLASH_INT_H
#include <stdint.h>
void flash_int_erase_page(uint32_t addr);
void flash_int_program_halfword(uint32_t addr, uint16_t data);
void flash_int_erase_area(uint32_t start, uint32_t size_kb);
#endif
