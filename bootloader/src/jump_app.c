#include "jump_app.h"
#include "stm32f10x.h"

void jump_to_app(void) {
    uint32_t app_sp = *(volatile uint32_t*)APP_START_ADDR;
    uint32_t app_pc = *(volatile uint32_t*)(APP_START_ADDR + 4);

    if (app_sp < RAM_START || app_sp >= RAM_END) return;
    if (app_pc < FLASH_START || app_pc >= FLASH_END) return;
    if ((app_pc & 1) == 0) return;

    __disable_irq();
    SCB->VTOR = APP_START_ADDR;
    __set_MSP(app_sp);
    ((void (*)(void))(app_pc))();
    while(1);
}
