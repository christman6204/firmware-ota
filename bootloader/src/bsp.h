/*
*********************************************************************************************************
*                                        BOARD SUPPORT PACKAGE
*
*                                        Bootloader (bare-metal)
*
* Filename      : bsp.h
*********************************************************************************************************
*/

#ifndef  BSP_PRESENT
#define  BSP_PRESENT

#include "stm32f10x.h"

/*
*********************************************************************************************************
*                                          LED DEFINITIONS
*********************************************************************************************************
*/

#define  LED_PORT               GPIOE
#define  LED_PIN                GPIO_Pin_0
#define  LED_RCC                RCC_APB2Periph_GPIOE
#define  LED_ON()               GPIO_ResetBits(LED_PORT, LED_PIN)   /* 低电平亮 */
#define  LED_OFF()              GPIO_SetBits(LED_PORT, LED_PIN)     /* 高电平灭 */
#define  LED_TOGGLE()           GPIO_WriteBit(LED_PORT, LED_PIN,    \
                                   (BitAction)(1 - GPIO_ReadOutputDataBit(LED_PORT, LED_PIN)))


/*
*********************************************************************************************************
*                                      JUMP TO APPLICATION
*********************************************************************************************************
*/

#define  APP_ADDRESS             0x0800C000u   /* App 固件起始地址（与设计文档一致） */


/*
*********************************************************************************************************
*                                           FUNCTION PROTOTYPES
*********************************************************************************************************
*/

void  BSP_Init      (void);
void  BSP_LED_Init  (void);
void  JumpToApp     (void);
void  SoftReset     (void);

#endif
