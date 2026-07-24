/*
*********************************************************************************************************
*                                      STM32F10x Peripheral Config
*                                        Bootloader (bare-metal)
*********************************************************************************************************
*/

#ifndef  __STM32F10x_CONF_H
#define  __STM32F10x_CONF_H

/* 只启用 Bootloader 需要的外设（节省编译时间） */
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_flash.h"
#include "stm32f10x_misc.h"

#ifdef  USE_FULL_ASSERT
  #define assert_param(expr) ((expr) ? (void)0 : assert_failed((uint8_t *)__FILE__, __LINE__))
  void assert_failed(uint8_t* file, uint32_t line);
#else
  #define assert_param(expr) ((void)0)
#endif

#endif
