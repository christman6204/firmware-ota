/**
 * @file    jump_app.c
 * @brief   Bootloader 跳转到应用程序实现
 *
 * @details 实现 Cortex-M3 (STM32F103VE) 从 Bootloader 跳转到
 *          用户应用程序的标准流程。
 *
 *          ARM Cortex-M3 向量表布局 (位于 APP_START_ADDR):
 *          - 偏移 0x00: 初始栈指针 (MSP) — 指向 RAM 顶部
 *          - 偏移 0x04: 复位向量 (Reset_Handler 地址) — 必须 bit[0]=1 (Thumb)
 *
 *          三重安全校验 (全部通过才执行跳转):
 *
 *          [校验 1] 栈指针范围校验:
 *            确保 app_sp 落入合法 SRAM 区间 (0x20000000 ~ 0x20010000)。
 *            栈指针指向 RAM 顶部，必须在此范围内。
 *
 *          [校验 2] 入口地址范围校验:
 *            确保 app_pc 落入合法 Flash 区间 (0x08000000 ~ 0x08080000)。
 *            防止跳转到无效地址导致 HardFault。
 *
 *          [校验 3] Thumb 位校验:
 *            Cortex-M 只支持 Thumb 指令集，所有跳转地址的 bit[0] 必须
 *            为 1 (EPSR.T = 1)。bit[0]=0 会导致 INVSTATE UsageFault。
 *
 *          @note  三重校验缺一不可，任一项失败即静默返回。
 *
 *          跳转流程:
 *          1. __disable_irq()     禁止全局中断
 *          2. SCB->VTOR = APP_START_ADDR  重定位向量表偏移寄存器
 *          3. __set_MSP(app_sp)  设置主栈指针为应用程序的栈顶
 *          4. 通过函数指针跳转到 Reset_Handler
 *
 *          @attention  跳转后不返回 (while(1) 兜底保护)。
 *          @attention  VTOR 重定位后，应用程序的中断向量表才生效。
 *                      CMSIS __set_MSP / __disable_irq 是编译器内置函数，
 *                      无需额外头文件。
 */

#include "jump_app.h"
#include "stm32f10x.h"

/**
 * @brief   跳转到用户应用程序
 *
 *          从 APP_START_ADDR (0x0800C000) 读取向量表前两个 32-bit 字:
 *          - offset 0: app_sp — 应用程序期望的初始 MSP
 *          - offset 4: app_pc — 应用程序的 Reset_Handler 地址
 *
 *          三重安全校验全部通过后才执行跳转，任何一项失败即返回。
 *          跳转成功后永不返回 (通过 while(1) 兜底)。
 */
void jump_to_app(void) {
    /* ---- 读取应用程序向量表 ---- */
    uint32_t app_sp = *(volatile uint32_t*)APP_START_ADDR;
    uint32_t app_pc = *(volatile uint32_t*)(APP_START_ADDR + 4);

    /* ---- 三重安全校验 ---- */

    /* [校验 1] 栈指针必须在合法 RAM 范围 (0x20000000 ~ 0x20010000) */
    if (app_sp < RAM_START || app_sp >= RAM_END) return;

    /* [校验 2] 入口地址必须在合法 Flash 范围 (0x08000000 ~ 0x08080000) */
    if (app_pc < FLASH_START || app_pc >= FLASH_END) return;

    /* [校验 3] Thumb 位校验: Cortex-M 只执行 Thumb 指令, bit[0] 必须为 1 */
    if ((app_pc & 1) == 0) return;

    /* ---- 执行跳转 (此后不返回) ---- */

    /* 禁止所有全局中断，防止跳转过程中被中断打断导致状态不一致 */
    __disable_irq();

    /* 重定位向量表到应用程序的向量表地址
     * 此后发生的中断/异常将使用应用程序的中断向量表 */
    SCB->VTOR = APP_START_ADDR;

    /* 设置主栈指针为应用程序期望的栈顶值
     * 注意: 必须在跳转前设置，否则 Reset_Handler 可能使用错误的栈 */
    __set_MSP(app_sp);

    /* 通过函数指针跳转到应用程序的 Reset_Handler
     * Cortex-M 的 BX 指令会根据 bit[0] 自动切换到 Thumb 状态 */
    ((void (*)(void))(app_pc))();

    /* 兜底: 如果跳转因某些极端原因返回，死循环防止执行后续未知代码 */
    while(1);
}
