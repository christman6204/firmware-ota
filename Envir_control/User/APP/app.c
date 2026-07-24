/*
*********************************************************************************************************
*                                              EXAMPLE CODE
*
*                          (c) Copyright 2003-2013; Micrium, Inc.; Weston, FL
*
*               All rights reserved.  Protected by international copyright laws.
*               Knowledge of the source code may NOT be used to develop a similar product.
*               Please help us continue to provide the Embedded community with the finest
*               software available.  Your honesty is greatly appreciated.
*********************************************************************************************************
*/

/*
*********************************************************************************************************
*                                            APPLICATION
*
*                                       STM32F103VE + uC/OS-III
*
* Filename      : app.c
* Description   : 干净 RTOS 底座 - 最小启动（原产品业务逻辑已剥离归档至 old_product/）
*                 后续在此框架上叠加 OTA + 数据采集任务
*********************************************************************************************************
*/

#include <includes.h>

/*
*********************************************************************************************************
*                                            LOCAL DEFINES
*********************************************************************************************************
*/


/*
*********************************************************************************************************
*                                                 TCB
*********************************************************************************************************
*/

OS_TCB   AppTaskStartTCB;

/*
*********************************************************************************************************
*                                                STACKS
*********************************************************************************************************
*/

__align(8) static  CPU_STK  AppTaskStartStk[APP_TASK_START_STK_SIZE];

/*
*********************************************************************************************************
*                                         FUNCTION PROTOTYPES
*********************************************************************************************************
*/

static  void  AppTaskStart  (void *p_arg);

/*
*********************************************************************************************************
*                                                main()
*
* Description : 标准入口。启动顺序：OSInit -> OSTaskCreate(起始任务) -> OSStart。
*               CPU_Init/Mem_Init/SysTick 等在起始任务（OSStart 之后）中初始化。
*********************************************************************************************************
*/

int  main (void)
{
    OS_ERR  err;

    OSInit(&err);                                  /* 初始化 uC/OS-III 内核                                  */

    OSTaskCreate((OS_TCB     *)&AppTaskStartTCB,
                 (CPU_CHAR   *)"App Task Start",
                 (OS_TASK_PTR ) AppTaskStart,
                 (void       *) 0,
                 (OS_PRIO     ) APP_TASK_START_PRIO,
                 (CPU_STK    *)&AppTaskStartStk[0],
                 (CPU_STK_SIZE) APP_TASK_START_STK_SIZE / 10,
                 (CPU_STK_SIZE) APP_TASK_START_STK_SIZE,
                 (OS_MSG_QTY  ) 5u,
                 (OS_TICK     ) 0u,
                 (void       *) 0,
                 (OS_OPT      )(OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR),
                 (OS_ERR     *)&err);

    OSStart(&err);                                 /* 启动多任务，不返回                                      */
}

/*
*********************************************************************************************************
*                                          STARTUP TASK
*
* Description : 起始任务。OSStart 后首次运行，负责：
*               BSP 初始化 -> CPU/内存初始化 -> SysTick 配置 -> 统计基准 -> 进入空循环。
*               后续在此任务内或其派生任务中叠加 OTA、数据采集等业务。
*********************************************************************************************************
*/

static  void  AppTaskStart (void *p_arg)
{
    OS_ERR   err;
    CPU_INT32U  cpu_clk_freq;
    CPU_INT32U  cnts;

    (void)p_arg;                                   /* 防止编译警告                                           */

    BSP_Init();                                    /* 板级初始化（当前为空，后续补 GPIO/UART/SPI 等）         */
    CPU_Init();                                    /* 初始化 CPU（时间戳/关中断测量）                         */

    cpu_clk_freq = BSP_CPU_ClkFreq();              /* HCLK = 72MHz                                           */
    cnts = cpu_clk_freq / (CPU_INT32U)OSCfg_TickRate_Hz;
    OS_CPU_SysTickInit(cnts);                      /* 配置 SysTick（必须在 OSStart 之后）                     */

    Mem_Init();                                    /* 初始化 uC/LIB 内存管理                                 */

#if OS_CFG_STAT_TASK_EN > 0u
    OSStatTaskCPUUsageInit(&err);                  /* 统计任务 CPU 利用率基准                                 */
#endif

    CPU_IntDisMeasMaxCurReset();                   /* 复位当前关中断时间峰值                                  */

    OSSchedRoundRobinCfg(DEF_ENABLED, 0, &err);    /* 使能时间片轮转（默认片长 TickRate/10）                  */

    while (1) {
        OSTimeDlyHMSM(0, 0, 1, 0,
                       OS_OPT_TIME_HMSM_STRICT,
                       &err);                      /* 1 秒延时，避免忙循环（后续替换为业务调度）              */
    }
}
