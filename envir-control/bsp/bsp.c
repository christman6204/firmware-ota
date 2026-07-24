/*
*********************************************************************************************************
*                                        BOARD SUPPORT PACKAGE
*
*                                       STM32F103VE + uC/OS-III
*
* Filename      : bsp.c
*********************************************************************************************************
*/

#define  BSP_MODULE
#include <bsp.h>


/*
*********************************************************************************************************
*                                            REGISTERS (DWT)
*********************************************************************************************************
*/

#define  DWT_CR      *(CPU_REG32 *)0xE0001000
#define  DWT_CYCCNT  *(CPU_REG32 *)0xE0001004
#define  DEM_CR      *(CPU_REG32 *)0xE000EDFC

#define  DEM_CR_TRCENA       (1 << 24)
#define  DWT_CR_CYCCNTENA    (1 <<  0)


/*
*********************************************************************************************************
*                                               BSP_Init()
*********************************************************************************************************
*/

void  BSP_Init (void)
{
    BSP_LED_Init();                    /* 初始化测试 LED（PE0）                    */
}


/*
*********************************************************************************************************
*                                            BSP_LED_Init()
*
* Description : 初始化 PE0 为推挽输出，初始灭（高电平）。
*               后续闪灯测试使用。
*********************************************************************************************************
*/

void  BSP_LED_Init (void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(LED_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin   = LED_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED_PORT, &GPIO_InitStructure);

    LED_OFF();                         /* 初始灭灯                                   */
}


/*
*********************************************************************************************************
*                                            SoftReset()
*********************************************************************************************************
*/

void SoftReset (void)
{
    CPU_IntDis();
    NVIC_SystemReset();
}


/*
*********************************************************************************************************
*                                            BSP_CPU_ClkFreq()
*********************************************************************************************************
*/

CPU_INT32U  BSP_CPU_ClkFreq (void)
{
    RCC_ClocksTypeDef  rcc_clocks;

    RCC_GetClocksFreq(&rcc_clocks);
    if (rcc_clocks.SYSCLK_Frequency != 72000000)
        SoftReset();

    return ((CPU_INT32U)rcc_clocks.HCLK_Frequency);
}


/*
*********************************************************************************************************
*                                          CPU_TS_TmrInit()
*********************************************************************************************************
*/

#if (CPU_CFG_TS_TMR_EN == DEF_ENABLED)
void  CPU_TS_TmrInit (void)
{
    CPU_INT32U  cpu_clk_freq_hz;

    DEM_CR     |= (CPU_INT32U)DEM_CR_TRCENA;
    DWT_CYCCNT  = (CPU_INT32U)0u;
    DWT_CR     |= (CPU_INT32U)DWT_CR_CYCCNTENA;

    cpu_clk_freq_hz = BSP_CPU_ClkFreq();
    CPU_TS_TmrFreqSet(cpu_clk_freq_hz);
}
#endif


/*
*********************************************************************************************************
*                                           CPU_TS_TmrRd()
*********************************************************************************************************
*/

#if (CPU_CFG_TS_TMR_EN == DEF_ENABLED)
CPU_TS_TMR  CPU_TS_TmrRd (void)
{
    return ((CPU_TS_TMR)DWT_CYCCNT);
}
#endif
