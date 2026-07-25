/**
 * @file    flash_ext.c
 * @brief   W25Q64 外部 SPI Flash 驱动（STM32F103 + 硬件 SPI1）
 *
 * @details
 * ## 芯片特性 (W25Q64JV / W25Q64FV)
 * | 参数           | 规格                                   |
 * |---------------|----------------------------------------|
 * | 总容量         | 8 MB (64 Mbit)                        |
 * | 扇区大小       | 4 KB (最小擦除单位)                     |
 * | 块大小 (32K)   | 32 KB                                  |
 * | 块大小 (64K)   | 64 KB                                  |
 * | 页大小         | 256 字节 (编程单位)                     |
 * | 擦除时间 (扇区) | 典型 45 ms, 最大 400 ms                |
 * | 编程时间 (页)   | 典型 0.7 ms, 最大 3 ms                 |
 * | SPI 模式       | Mode 0 (CPOL=0, CPHA=0) 或 Mode 3     |
 * | 电源           | 2.7V ~ 3.6V                            |
 *
 * ## SPI 引脚映射 (硬件 SPI1)
 * | STM32 引脚 | 功能  | 连接至 W25Q64 | 说明                          |
 * |-----------|------|--------------|------------------------------|
 * | PA5       | SCK  | CLK (6)      | 串行时钟, 最大支持 80 MHz     |
 * | PA6       | MISO | DO (2)       | 主入从出, 数据从 Flash 读出   |
 * | PA7       | MOSI | DI (5)       | 主出从入, 指令/地址/数据写入   |
 * | PB0       | CS   | /CS (1)      | 片选, 低电平有效               |
 *
 * ## W25Q64 指令说明
 * | 指令 | 编码    | 描述                                      |
 * |-----|--------|-------------------------------------------|
 * | 读数据 (Read Data) | `0x03` | 标准读取, 最大 50 MHz; 发送指令+3字节地址后,
 *   时钟线上继续输出 SCLK 即可从 MISO 连续读出数据 |
 * | 页编程 (Page Program) | `0x02` | 在已擦除区域写入最多 256 字节;
 *   地址自动递增, 但不可跨页 (跨页需分两次调用)  |
 * | 扇区擦除 (Sector Erase 4KB) | `0x20` | 将 4KB 扇区内所有位擦除为 0xFF |
 * | 读 JEDEC ID | `0x9F` | 返回厂商 ID + 内存类型 + 容量 (3 字节);
 *   W25Q64: EF4017 或类似, 用于确认 SPI 通信正常 |
 * | 读状态寄存器 (Read Status) | `0x05` | 返回 1 字节; bit0 = BUSY (1 = 忙);
 *   用于等待写/擦除完成                             |
 *
 * ## 外设配置要点
 * - SPI 时钟: 72 MHz / 4 = 18 MHz (APB2 72 MHz / prescaler 4)
 * - GPIO 模式: SCK/MISO/MOSI → 复用推挽输出 (AF_PP, 50 MHz)
 * - CS: 推挽输出, 初始高电平 (不选中)
 *
 * @author Christman
 * @date   2025
 */

/* ============================ 头文件 ============================ */
#include "flash_ext.h"
#include "stm32f10x_spi.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"

/* ============================ CS 引脚宏 ============================ */

/** @brief SPI 片选端口: GPIOB */
#define SPI_CS_PORT  GPIOB

/** @brief SPI 片选引脚: PB0 */
#define SPI_CS_PIN   GPIO_Pin_0

/* ============================ CS 控制内联函数 ============================ */

/**
 * @brief  拉低 CS, 选中 SPI Flash
 * @note   所有 SPI 指令必须在 CS 拉低后发送, 指令完成后拉高 CS
 */
static void cs_low(void)  { GPIO_ResetBits(SPI_CS_PORT, SPI_CS_PIN); }

/**
 * @brief  拉高 CS, 释放 SPI Flash 总线
 */
static void cs_high(void) { GPIO_SetBits(SPI_CS_PORT, SPI_CS_PIN); }

/* ============================ 底层 SPI 收发 ============================ */

/**
 * @brief  通过 SPI1 发送一字节并同时接收一字节（全双工）
 * @param  tx  要发送的字节
 * @return 接收到的字节 (uint8_t, 0x00-0xFF)
 * @note
 * - 阻塞式操作: 等待 TXE (发送缓冲区空) → 写入 DR → 等待 RXNE (接收非空) → 读取 DR
 * - SPI 全双工: 每发送一个字节的同时也收到一个字节; W25Q64 在读指令期间返回有效数据,
 *   其他指令返回通常是 0x00 或 0xFF
 */
static uint8_t spi_xfer(uint8_t tx) {
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET);
    SPI_I2S_SendData(SPI1, tx);
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET);
    return (uint8_t)SPI_I2S_ReceiveData(SPI1);
}

/* ============================ 初始化 ============================ */

/**
 * @brief  初始化外部 SPI Flash (W25Q64) 外设
 *
 * @details 初始化步骤:
 * 1. 使能外设时钟: SPI1、GPIOA (SCK/MISO/MOSI)、GPIOB (CS)
 * 2. 配置 PA5/PA6/PA7 为复用推挽输出 (AF_PP), 50 MHz
 *    - PA5: SCK (时钟)
 *    - PA6: MISO (主入从出)
 *    - PA7: MOSI (主出从入)
 * 3. 配置 PB0 为推挽输出 (Out_PP), 初始拉高 CS (不选中 Flash)
 * 4. 配置 SPI1:
 *    - 主模式 (Master)
 *    - 预分频 /4 → 72 MHz / 4 = 18 MHz SCLK
 *    - SPI Mode 0: CPOL = 0 (空闲低), CPHA = 1Edge (第一个边沿采样)
 *    - 全双工 (2 Lines Full Duplex)
 *    - 数据帧 8 位 (默认)
 *    - MSB 先发 (默认)
 * 5. 使能 SPI1 外设
 *
 * @note  调用时机: 系统上电后, 在首次访问 Flash 前调用一次即可
 * @note  本函数不检测 Flash 是否存在; 后续可通过 flash_ext_read_id() 验证
 */
void flash_ext_init(void) {
    GPIO_InitTypeDef g;
    SPI_InitTypeDef  s;

    /* ---- 步骤 1: 使能外设时钟 ---- */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1 | RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    /* ---- 步骤 2: 配置 SPI 信号引脚 (PA5/PA6/PA7) ---- */
    g.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
    g.GPIO_Mode = GPIO_Mode_AF_PP;           /* 复用推挽输出 */
    g.GPIO_Speed = GPIO_Speed_50MHz;          /* 高速 50 MHz */
    GPIO_Init(GPIOA, &g);

    /* ---- 步骤 3: 配置片选引脚 (PB0), 初始拉高 ---- */
    g.GPIO_Pin = GPIO_Pin_0;
    g.GPIO_Mode = GPIO_Mode_Out_PP;           /* 通用推挽输出 */
    GPIO_Init(GPIOB, &g);
    cs_high();                                /* 初始不选中 Flash */

    /* ---- 步骤 4: 配置 SPI1 外设 ---- */
    SPI_StructInit(&s);                       /* 先填充默认值 */
    s.SPI_Mode = SPI_Mode_Master;             /* 主模式 */
    s.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;  /* 时钟 18 MHz */
    s.SPI_CPOL = SPI_CPOL_Low;                /* CPOL=0: 空闲时 SCK 为低 */
    s.SPI_CPHA = SPI_CPHA_1Edge;              /* CPHA=0: 第 1 个时钟边沿采样 (SPI Mode 0) */
    s.SPI_Direction = SPI_Direction_2Lines_FullDuplex;   /* 全双工 */
    SPI_Init(SPI1, &s);

    /* ---- 步骤 5: 使能 SPI1 ---- */
    SPI_Cmd(SPI1, ENABLE);
}

/* ============================ 读操作 ============================ */

/**
 * @brief  从外部 Flash 读取连续数据
 * @param  addr  起始地址 (0x000000 ~ 0x7FFFFF, 24 位有效)
 * @param  buf   读取数据存放的缓冲区 (调用者分配)
 * @param  len   要读取的字节数
 *
 * @details
 * 使用 0x03 (Read Data) 指令:
 * ```
 * CS ─┐        ┌──────────────────────────┐
 *     └────────┘                          └──────────
 * DI:    [0x03] [ADDR:24]  (无需数据输入, MISO 输出数据)
 * DO:                      [D0] [D1] [D2] ... [Dn]
 * ```
 * - 发送 0x03 指令 → 发送 3 字节地址 (MSB 优先)
 * - 持续发送 0xFF (哑字节) 以产生 SCLK, 同时读取 MISO 上的数据
 * - 地址自动递增, 可连续跨页/跨扇区读取
 *
 * @note  无需在调用前擦除 (Flash 读操作无限制)
 * @note  addr 仅低 24 位有效 (W25Q64 地址范围 0x000000 ~ 0x7FFFFF)
 */
void flash_ext_read(uint32_t addr, uint8_t *buf, uint32_t len) {
    cs_low();

    /* 发送读指令 + 24 位起始地址 (MSB 优先) */
    spi_xfer(0x03);
    spi_xfer((addr >> 16) & 0xFF);
    spi_xfer((addr >> 8)  & 0xFF);
    spi_xfer( addr        & 0xFF);

    /* 发送哑字节 (0xFF) 产生时钟, 同时读取 MISO 数据 */
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = spi_xfer(0xFF);
    }

    cs_high();
}

/* ============================ 忙状态检测 ============================ */

/**
 * @brief  等待 Flash 内部写/擦除操作完成（阻塞）
 *
 * @details
 * 发送 0x05 (Read Status Register-1) 指令, 读取状态寄存器, 轮询 bit0 (BUSY):
 * - BUSY = 1: Flash 正在执行内部写/擦除/编程操作, 不可接受新指令
 * - BUSY = 0: Flash 空闲, 可以接受新指令
 *
 * @note  阻塞式等待, 擦除扇区时可能阻塞数十毫秒
 * @note  必须在每次页编程或扇区擦除后调用, 否则后续操作会失败
 */
static void wait_busy(void) {
    cs_low();
    spi_xfer(0x05);                   /* 读状态寄存器指令 */
    while (spi_xfer(0xFF) & 0x01);    /* 轮询 bit0 (BUSY), 等待变为 0 */
    cs_high();
}

/* ============================ 写操作 ============================ */

/**
 * @brief  向外部 Flash 写入数据（先擦除扇区, 再页编程）
 * @param  addr  写入起始地址 (0x000000 ~ 0x7FFFFF)
 * @param  buf   要写入的数据缓冲区
 * @param  len   要写入的字节数
 *
 * @details
 * 操作流程:
 * 1. 先擦除目标地址所在 4KB 扇区 (flash_ext_erase_sector)
 *    - 地址按 4KB 对齐取整: `addr & 0xFFFFF000`
 * 2. 等待擦除完成 (wait_busy)
 * 3. 发送 0x02 (Page Program) 指令 + 24 位地址
 * 4. 连续发送数据字节 (最多 256 字节)
 * 5. 等待编程完成 (wait_busy)
 *
 * @note  **重要限制**:
 * - 本函数会擦除整个 4KB 扇区, 不保留扇区内其他数据!
 *   如需保留, 调用者应先读出扇区内容, 修改后在写入
 * - 单次写入不应超过 256 字节 (W25Q64 页大小限制); 跨页写需分多次调用
 * - 写地址必须在已擦除状态 (0xFF), 否则写入位可能出错
 */
void flash_ext_write_page(uint32_t addr, const uint8_t *buf, uint32_t len) {
    /* 步骤 1-2: 先擦除扇区并等待完成 */
    flash_ext_erase_sector(addr & 0xFFFFF000);
    wait_busy();

    /* 步骤 3-4: 发送页编程指令 + 地址 + 数据 */
    cs_low();
    spi_xfer(0x02);                         /* 页编程指令 */
    spi_xfer((addr >> 16) & 0xFF);          /* 地址 A23-A16 */
    spi_xfer((addr >> 8)  & 0xFF);          /* 地址 A15-A8 */
    spi_xfer( addr        & 0xFF);          /* 地址 A7-A0 */
    for (uint32_t i = 0; i < len; i++) {
        spi_xfer(buf[i]);                   /* 数据字节 */
    }
    cs_high();

    /* 步骤 5: 等待编程完成 */
    wait_busy();
}

/* ============================ 擦除操作 ============================ */

/**
 * @brief  擦除一个 4KB 扇区
 * @param  addr  扇区内的任意地址 (自动按 4KB 对齐: 取 addr 低 12 位为 0)
 *
 * @details
 * 使用 0x20 (Sector Erase 4KB) 指令:
 * ```
 * CS ─┐                          ┌──────
 *     └──────────────────────────┘
 * DI:    [0x20] [ADDR:24]
 * ```
 * - 发送 0x20 指令 + 24 位地址
 * - 擦除后扇区内所有字节变为 0xFF
 * - 等待 BUSY 位清零后返回
 *
 * @note  擦除时间: 典型 45 ms, 最大 400 ms (W25Q64 数据手册)
 * @note  扇区号 = addr / 4096; W25Q64 共有 2048 个 4KB 扇区
 */
void flash_ext_erase_sector(uint32_t addr) {
    cs_low();

    /* 发送扇区擦除指令 + 24 位地址 */
    spi_xfer(0x20);
    spi_xfer((addr >> 16) & 0xFF);
    spi_xfer((addr >> 8)  & 0xFF);
    spi_xfer( addr        & 0xFF);

    cs_high();

    /* 等待内部擦除完成 */
    wait_busy();
}

/**
 * @brief  擦除一个 4KB 扇区 (flash_ext_erase_sector 的别名)
 * @param  addr  扇区内任意地址
 * @see    flash_ext_erase_sector
 */
void flash_ext_erase_4k(uint32_t addr) {
    flash_ext_erase_sector(addr);
}

/* ============================ 设备识别 ============================ */

/**
 * @brief  读取 JEDEC 制造商/设备 ID (3 字节)
 * @return 24 位 JEDEC ID:
 *         - bit[23:16] = 厂商 ID (Winbond = 0xEF)
 *         - bit[15:8]  = 内存类型 (W25Q64 = 0x40)
 *         - bit[7:0]   = 容量代码 (W25Q64 = 0x17 for 64Mbit, 或 0x16)
 *
 * @details
 * 使用 0x9F (JEDEC ID) 指令:
 * ```
 * CS ─┐                ┌──────────
 *     └────────────────┘
 * DI:    [0x9F]
 * DO:                [MID] [DID] [JDID]
 * ```
 * - 发送 0x9F 指令
 * - 读取 3 字节: Manufacturer ID + Memory Type + Capacity
 *
 * @note  典型用途: Bootloader 初始化后读取 ID 校验 SPI 通信是否正常
 * @note  W25Q64 预期返回值: 0xEF4017 或 0xEF4016 (视具体版本)
 */
uint32_t flash_ext_read_id(void) {
    uint32_t id;
    cs_low();

    /* 发送 JEDEC ID 指令 */
    spi_xfer(0x9F);

    /* 读取 3 字节: 厂商 ID | 内存类型 | 容量 */
    id = ((uint32_t)spi_xfer(0xFF) << 16)
       | ((uint32_t)spi_xfer(0xFF) << 8)
       |  spi_xfer(0xFF);

    cs_high();
    return id;
}
