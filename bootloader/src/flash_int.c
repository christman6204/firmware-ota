/**
 * @file    flash_int.c
 * @brief   STM32 片内 Flash 操作实现
 *
 * @details STM32F103VE 片内 Flash 关键参数:
 *          - 总容量: 512 KB, 地址空间 0x08000000 ~ 0x0807FFFF
 *          - 页大小: 2 KB (2048 字节), 共 256 页
 *          - 编程位宽: 16-bit (半字), 不支持字节写入
 *
 *          标准 FLASH 操作序列:
 *          1. FLASH_Unlock()           解除写保护
 *          2. FLASH_ClearFlag()        清除挂起的错误标志
 *          3. FLASH_ErasePage() / FLASH_ProgramHalfWord()   执行擦除或编程
 *          4. FLASH_Lock()             恢复写保护
 *
 * @attention  FLASH_ErasePage(addr) 的参数是页内任意地址，
 *             硬件会自动将其对齐到所在页 (2 KB 边界) 的起始地址。
 * @attention  擦除是页为单位的，即使是 2 字节的数据，也需要先擦除整个页。
 */

#include "flash_int.h"
#include "stm32f10x_flash.h"

/**
 * @brief   擦除片内 Flash 的一个页 (2 KB)
 *
 *          标准操作序列:
 *          1. Unlock:  解除 Flash 控制寄存器的写保护锁
 *          2. ClearFlag: 清除 EOP / PGERR / WRPRTERR 三种挂起标志,
 *             防止上次操作残留的状态干扰本次操作
 *          3. ErasePage: 擦除目标页，参数为页内任意地址
 *          4. Lock: 恢复写保护，防止误操作
 *
 * @param   addr  页内任意地址，硬件自动对齐到该页起始地址
 */
void flash_int_erase_page(uint32_t addr) {
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ErasePage(addr);
    FLASH_Lock();
}

/**
 * @brief   向片内 Flash 写入一个半字 (16-bit)
 *
 *          STM32F1 系列片内 Flash 仅支持 16-bit 编程，不能单字节写入。
 *          因此在写入前需确保目标地址已处于擦除态 (全 0xFFFF)，
 *          否则写入将失败 (由 PGERR 标志指示)。
 *
 *          操作序列与 flash_int_erase_page 一致，
 *          仅将 FLASH_ErasePage 替换为 FLASH_ProgramHalfWord。
 *
 * @param   addr  目标地址 (必须为偶数，半字对齐)
 * @param   data  要写入的 16-bit 数据
 */
void flash_int_program_halfword(uint32_t addr, uint16_t data) {
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ProgramHalfWord(addr, data);
    FLASH_Lock();
}

/**
 * @brief   擦除片内 Flash 的一段连续区域
 *
 *          按 2 KB 页步进，从 start 开始逐页擦除直到覆盖 end 地址。
 *          end = start + size_kb * 1024
 *
 *          注意: 每个 flash_int_erase_page 内部都包含
 *          独立的 Unlock / ClearFlag / Erase / Lock 序列，
 *          因此擦除大区域时会有多次 Lock/Unlock 开销，
 *          但对于 Bootloader 场景 (参数页、固件区) 性能可接受。
 *
 * @param   start    起始地址 (页内任意地址)
 * @param   size_kb  要擦除的大小，单位 KB
 */
void flash_int_erase_area(uint32_t start, uint32_t size_kb) {
    uint32_t end = start + (size_kb * 1024u);
    for (uint32_t addr = start; addr < end; addr += 2048u) {
        flash_int_erase_page(addr);
    }
}
