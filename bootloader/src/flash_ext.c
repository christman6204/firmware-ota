#include "flash_ext.h"
#include "stm32f10x_spi.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"

#define SPI_CS_PORT  GPIOB
#define SPI_CS_PIN   GPIO_Pin_0

static void cs_low(void)  { GPIO_ResetBits(SPI_CS_PORT, SPI_CS_PIN); }
static void cs_high(void) { GPIO_SetBits(SPI_CS_PORT, SPI_CS_PIN); }

static uint8_t spi_xfer(uint8_t tx) {
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET);
    SPI_I2S_SendData(SPI1, tx);
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET);
    return (uint8_t)SPI_I2S_ReceiveData(SPI1);
}

void flash_ext_init(void) {
    GPIO_InitTypeDef g;
    SPI_InitTypeDef  s;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1 | RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);
    g.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
    g.GPIO_Mode = GPIO_Mode_AF_PP; g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &g);
    g.GPIO_Pin = GPIO_Pin_0; g.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOB, &g); cs_high();
    SPI_StructInit(&s);
    s.SPI_Mode = SPI_Mode_Master; s.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
    s.SPI_CPOL = SPI_CPOL_Low; s.SPI_CPHA = SPI_CPHA_1Edge;
    s.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    SPI_Init(SPI1, &s); SPI_Cmd(SPI1, ENABLE);
}

void flash_ext_read(uint32_t addr, uint8_t *buf, uint32_t len) {
    cs_low();
    spi_xfer(0x03); spi_xfer((addr>>16)&0xFF); spi_xfer((addr>>8)&0xFF); spi_xfer(addr&0xFF);
    for (uint32_t i=0; i<len; i++) buf[i] = spi_xfer(0xFF);
    cs_high();
}

static void wait_busy(void) {
    cs_low(); spi_xfer(0x05);
    while (spi_xfer(0xFF) & 0x01); cs_high();
}

void flash_ext_write_page(uint32_t addr, const uint8_t *buf, uint32_t len) {
    flash_ext_erase_sector(addr & 0xFFFFF000);
    wait_busy();
    cs_low();
    spi_xfer(0x02); spi_xfer((addr>>16)&0xFF); spi_xfer((addr>>8)&0xFF); spi_xfer(addr&0xFF);
    for (uint32_t i=0; i<len; i++) spi_xfer(buf[i]);
    cs_high(); wait_busy();
}

void flash_ext_erase_sector(uint32_t addr) {
    cs_low();
    spi_xfer(0x20); spi_xfer((addr>>16)&0xFF); spi_xfer((addr>>8)&0xFF); spi_xfer(addr&0xFF);
    cs_high(); wait_busy();
}

void flash_ext_erase_4k(uint32_t addr) { flash_ext_erase_sector(addr); }

uint32_t flash_ext_read_id(void) {
    uint32_t id;
    cs_low(); spi_xfer(0x9F);
    id = ((uint32_t)spi_xfer(0xFF) << 16) | ((uint32_t)spi_xfer(0xFF) << 8) | spi_xfer(0xFF);
    cs_high();
    return id;
}
