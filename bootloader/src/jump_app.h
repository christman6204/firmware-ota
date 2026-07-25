/**
 * @file    jump_app.h
 * @brief   Bootloader 跳转到应用程序
 *
 * @details 定义从 Bootloader 跳转到用户应用程序所需的内存地址
 *          常量和跳转函数接口。
 *
 *          芯片: STM32F103VE (Cortex-M3)
 *          - SRAM:  0x20000000 ~ 0x20010000 (64 KB)
 *          - Flash: 0x08000000 ~ 0x08080000 (512 KB)
 *
 *          布局:
 *          - Bootloader:  0x08000000 ~ 0x0800BFFF (48 KB)
 *          - Application: 0x0800C000 ~ 0x0807FFFF (剩余 ~464 KB)
 *
 * @note    跳转前必须进行三重校验，确保目标地址有效后才执行跳转。
 */

#ifndef JUMP_APP_H
#define JUMP_APP_H

#include <stdint.h>

/** 应用程序 Flash 起始地址 (向量表首地址) */
#define APP_START_ADDR  0x08010000u

/** SRAM 基地址 (0x0800C000)，用于校验栈指针是否在合法 RAM 范围 */
#define RAM_START       0x20000000u

/** SRAM 结束地址 (64 KB)，用于校验栈指针是否在合法 RAM 范围 */
#define RAM_END         0x20010000u

/** Flash 起始地址，用于校验入口地址是否在合法 Flash 范围 */
#define FLASH_START     0x08000000u

/** Flash 结束地址 (512 KB)，用于校验入口地址是否在合法 Flash 范围 */
#define FLASH_END       0x08080000u

/**
 * @brief   跳转到应用程序入口
 *
 *          执行流程:
 *          1. 从 APP_START_ADDR 读取初始化栈指针 (MSP 初始值)
 *          2. 从 APP_START_ADDR + 4 读取复位向量 (Reset_Handler 地址)
 *          3. 三重校验: 栈指针范围 / 入口地址范围 / Thumb 位
 *          4. 禁止全局中断
 *          5. 重定位向量表 (SCB->VTOR)
 *          6. 设置主栈指针 (MSP)
 *          7. 跳转到复位向量 (永不返回)
 *
 * @note    此函数仅在验证通过时执行跳转，失败静默返回。
 * @note    跳转后不会返回，跳转失败会返回调用方。
 * @attention  禁止在跳转前使能任何可能触发的中断。
 */
void jump_to_app(void);

#endif
