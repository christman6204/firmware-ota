/* bsp_adapter.h
 *
 * SD 卡 SPI 驱动适配层 —— 将 downloader 的 bsp_spi_sd.c 依赖的
 * bsp_gpio.h / bsp_usart.h 抽象为本地适配接口。
 *
 * 引脚定义（本工程引脚分配表）:
 *   - SPI2: SCK=PB13, MISO=PB14, MOSI=PB15, CS=PB11
 *
 * 调试打印: 通过调试串口 (USART1) 或空实现。
 *   当前默认空实现（静默），后续接入调试串口后改为真实打印。
 */
#ifndef BSP_ADAPTER_H
#define BSP_ADAPTER_H

#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"

/* ---- SD 卡 CS 引脚 (本工程引脚分配: PB11) ---- */
#define SD_CS_PORT      GPIOB
#define SD_CS_PIN       GPIO_Pin_11
#define SD_CS_RCC       RCC_APB2Periph_GPIOB

/* ---- CS 控制 (内联) ---- */
static inline void BSP_SD_CS_Low(void)  { GPIO_ResetBits(SD_CS_PORT, SD_CS_PIN); }
static inline void BSP_SD_CS_High(void) { GPIO_SetBits(SD_CS_PORT, SD_CS_PIN); }

/* ---- 调试打印 (当前空实现, 避免依赖 bsp_usart) ---- */
#if 1   /* 1 = 静默, 0 = 打印 */
#define BSP_USART2_Printf(...)   ((void)0)
#else
#include <stdio.h>
#define BSP_USART2_Printf        printf
#endif

#endif /* BSP_ADAPTER_H */
