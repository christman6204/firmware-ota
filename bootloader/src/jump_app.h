#ifndef JUMP_APP_H
#define JUMP_APP_H
#include <stdint.h>

#define APP_START_ADDR  0x0800C000u
#define RAM_START       0x20000000u
#define RAM_END         0x20010000u
#define FLASH_START     0x08000000u
#define FLASH_END       0x08080000u

void jump_to_app(void);
#endif
