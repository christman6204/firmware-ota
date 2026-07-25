/**
 * @file    flash_int.h
 * @brief   STM32 片内 Flash 操作接口
 *
 * @details 封装 STM32F103VE 片内 Flash 的擦除与编程操作。
 *          - Flash 总容量: 512 KB (0x08000000 ~ 0x0807FFFF)
 *          - 页大小: 2 KB (2048 字节)
 *          - 编程位宽: 16-bit (半字)
 *
 *          所有操作遵循标准 FLASH 序列:
 *          FLASH_Unlock() -> FLASH_ClearFlag() -> 操作 -> FLASH_Lock()
 *
 * @note    FLASH_ErasePage 的参数是页内任意地址，硬件自动对齐到页首。
 * @note    本模块仅提供擦页和半字编程两个原子操作，不直接对外暴露
 *          Unlock/Lock，由调用方组合使用。
 */

#ifndef FLASH_INT_H
#define FLASH_INT_H

#include <stdint.h>

/**
 * @brief   擦除片内 Flash 的一个页 (2 KB)
 * @param   addr  页内任意地址，硬件自动对齐到该页的起始地址
 * @note    内部执行 Unlock -> ClearFlag -> ErasePage -> Lock 序列
 */
void flash_int_erase_page(uint32_t addr);

/**
 * @brief   向片内 Flash 写入一个半字 (16-bit)
 * @param   addr  目标地址 (必须对齐到半字，即偶数地址)
 * @param   data  要写入的 16-bit 数据
 * @note    内部执行 Unlock -> ClearFlag -> ProgramHalfWord -> Lock 序列
 * @note    STM32F1 片内 Flash 仅支持 16-bit 编程，不能按字节写入
 */
void flash_int_program_halfword(uint32_t addr, uint16_t data);

/**
 * @brief   擦除片内 Flash 的一段连续区域
 * @param   start    起始地址 (页内任意地址，自动对齐到首页)
 * @param   size_kb  要擦除的大小 (单位: KB)
 * @note    内部按 2 KB 页循环调用 flash_int_erase_page
 * @note    会擦除覆盖 start ~ start + size_kb * 1024 的全部页
 */
void flash_int_erase_area(uint32_t start, uint32_t size_kb);

#endif
