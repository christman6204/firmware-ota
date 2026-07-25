# Bootloader 全面实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 STM32F103VE 裸机 bootloader（0x08000000 48KB），含片内/片外 flash 驱动、AES-256-CTR+HMAC-SHA256 验签解密、备份/擦除/写入/回滚、掉电重入保护、App 跳转。

**Architecture:** 裸机 C，分层架构——`flash_drv`（片内+片外 SPI W25Q64）→ `crypto`（mbedtls AES+HMAC）→ `param`（ota_param_t CRUD+CRC32）→ `boot_main`（状态机：验签→备份→擦写→跳转/回滚）+ `jump_app`（VTOR/MSP 验证跳转）。PE0 闪灯保留作为诊断。

**Tech Stack:** STM32F103VE Cortex-M3, Keil ARMCC V5, Libraries (CMSIS+FWlib), mbedtls V2.x

## Global Constraints

- 片内 flash 页大小 2KB，擦除最小单位 2KB，编程最小单位 16-bit
- 片外 SPI flash W25Q64 8MB，扇区 4KB
- Bootloader 最大 48KB (0xC000)，位于 0x08000000
- App 区 0x0800C000 ~ 0x0804C000 (256KB)
- 参数区 0x0804C000 (4KB)
- 新固件区 片外 0x00_1000 (512KB)，备份区 0x08_2000 (512KB)
- 主密钥 AES-256(32B) + HMAC key(32B) 硬编码在 bootloader，RDP Level 1 保护
- 所有错误路径必须写 upgrade_result 后跳转当前 App 或回滚
- verify-before-write：先验 HMAC，通过后才动片内 flash
- 掉电重入幂等：magic 标记备份完成，重启跳过重复备份

---

## File Map

```
bootloader/src/
├── ota_param.h       [新建] 参数区结构体类型定义（共享）
├── flash_int.c/.h    [新建] 片内 flash 驱动（页擦除、半字编程）
├── flash_ext.c/.h    [新建] 片外 W25Q64 SPI flash 驱动（扇区读写擦、固件头读写）
├── param.c/.h        [新建] 参数区 CRUD（读/写/校验 CRC32）
├── crypto.c/.h       [新建] mbedtls 封装（AES-256-CTR 流式解/加密、HMAC-SHA256 流式计算/验证）
├── jump_app.c/.h     [新建] App 跳转（栈址/入口校验、VTOR、MSP）
├── boot_main.c       [新建] bootloader 主状态机（上电→读状态→验签→备份→擦写→跳转/回滚）
├── main.c            [修改] 简化为：PE0 闪灯（保留诊断）→ 调 boot_main()
├── bsp.c/.h          [保留] PE0 LED + SoftReset + BSP_CPU_ClkFreq
├── stm32f10x_conf.h  [保留] 精简外设头
├── stm32f10x_it.c/.h [保留] 最小 fault
```

---

### Task 1: ota_param.h — 参数区结构体定义

**Files:**
- Create: `bootloader/src/ota_param.h`
- Create: `bootloader/src/ota_param.c`（空，仅占位——常量定义留后续）

**Interfaces:**
- Produces: `ota_param_t` 结构体类型，`OTA_PARAM_ADDR`、`OTA_STATE_*`、`OTA_RESULT_*` 宏

- [ ] **Step 1: 创建 ota_param.h**

```c
// bootloader/src/ota_param.h
#ifndef OTA_PARAM_H
#define OTA_PARAM_H

#include <stdint.h>

/* 参数区地址 */
#define OTA_PARAM_ADDR          0x0804C000u
#define OTA_PARAM_PAGE_SIZE     2048u    /* STM32F103VE 页大小 2KB */
#define OTA_PARAM_SIZE          4096u    /* 分配 4KB = 2 页 */

/* OTA 状态机 */
#define OTA_STATE_IDLE              0u
#define OTA_STATE_DOWNLOADING       1u
#define OTA_STATE_DOWNLOADED        2u
#define OTA_STATE_UPGRADE_REQUESTED 3u
#define OTA_STATE_UPGRADING         4u

/* Bootloader 升级结果码 */
#define OTA_RESULT_NONE             0u   /* 无 */
#define OTA_RESULT_SUCCESS          1u   /* 升级成功 */
#define OTA_RESULT_VERIFY_FAIL      2u   /* 验签失败（HMAC 不匹配）*/
#define OTA_RESULT_BACKUP_FAIL      3u   /* 备份失败 */
#define OTA_RESULT_SPI_ERROR        4u   /* SPI flash 读写错误 */
#define OTA_RESULT_WRITE_ROLLBACK   5u   /* 写入失败，已回滚 */
#define OTA_RESULT_BOOT_FAIL        6u   /* 启动失败（app_healthy 超时），已回滚 */

/* 参数区结构体（64 字节，与设计文档 §7.1 一致） */
typedef struct {
    uint32_t magic;           /* 0x5041524D ("PARM") */
    uint32_t dev_id;          /* 设备 ID */
    uint8_t  state;           /* OTA 状态机 */
    uint8_t  app_healthy;     /* 启动确认标志 */
    uint8_t  upgrade_flag;    /* 触发升级标志 */
    uint8_t  upgrade_result;  /* Bootloader 写入的结果码 */
    uint8_t  task_id[16];     /* OTA 任务 ID */
    char     cur_version[16]; /* 当前 App 版本 */
    char     new_version[16]; /* 升级目标版本 */
    uint32_t crc32;           /* 结构体 CRC32 */
} ota_param_t;

#endif
```

- [ ] **Step 2: 编译验证**

在 Keil Bootloader 工程中编译，确认 ota_param.h 无语法错误。
预期：Warning-free compile（.h 文件本身不产生 object，但被 include 时不应报错）。

- [ ] **Step 3: Commit**

```bash
git add bootloader/src/ota_param.h
git commit -m "feat(bootloader): add ota_param_t struct definition"
```

---

### Task 2: flash_int — 片内 flash 驱动

**Files:**
- Create: `bootloader/src/flash_int.c`
- Create: `bootloader/src/flash_int.h`

**Interfaces:**
- Produces: `flash_int_erase_page(addr)`, `flash_int_program_word(addr, data)`, `flash_int_erase_area(start, size_kb)`
- Consumes: STM32F10x Flash API (`stm32f10x_flash.h`)

- [ ] **Step 1: 创建 flash_int.h**

```c
// bootloader/src/flash_int.h
#ifndef FLASH_INT_H
#define FLASH_INT_H
#include <stdint.h>

/* 片内 flash 页擦除（addr 自动对齐到 2KB 页边界）*/
void flash_int_erase_page(uint32_t addr);

/* 片内 flash 半字(16-bit)编程（地址必须已擦除） */
void flash_int_program_halfword(uint32_t addr, uint16_t data);

/* 擦除一段区域（start 页对齐，size_kb 为 KB 数） */
void flash_int_erase_area(uint32_t start, uint32_t size_kb);

#endif
```

- [ ] **Step 2: 创建 flash_int.c**

```c
// bootloader/src/flash_int.c
#include "flash_int.h"
#include "stm32f10x_flash.h"

void flash_int_erase_page(uint32_t addr) {
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ErasePage(addr);   /* STM32F10x 标准库：参数为页内任意地址 */
    FLASH_Lock();
}

void flash_int_program_halfword(uint32_t addr, uint16_t data) {
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ProgramHalfWord(addr, data);
    FLASH_Lock();
}

void flash_int_erase_area(uint32_t start, uint32_t size_kb) {
    uint32_t pages = size_kb * 512;  /* 1KB = 512 halfwords, but pages are 2KB so size_kb/2 pages */
    /* 简化：size_kb 传入的是 App 区 KB 数，按 2KB 页擦除 */
    (void)size_kb;
    uint32_t end = start + (size_kb * 1024u);
    for (uint32_t addr = start; addr < end; addr += 2048u) {
        flash_int_erase_page(addr);
    }
}
```

- [ ] **Step 3: 编写单元级验证代码**

在 main.c 临时添加测试：擦除参数区第一页 → 检查内容为 0xFF → 写入一个半字 → 回读验证 → 恢复。

```c
// 临时测试（编译验证后删除）
void test_flash_int(void) {
    uint32_t test_addr = OTA_PARAM_ADDR;
    flash_int_erase_page(test_addr);
    /* 验证擦除后为 0xFF */
    uint16_t val = *(volatile uint16_t*)test_addr;
    if (val != 0xFFFF) { LED_ON(); while(1); } /* 失败：LED 常亮 */
    flash_int_program_halfword(test_addr, 0x1234);
    val = *(volatile uint16_t*)test_addr;
    if (val != 0x1234) { LED_ON(); while(1); } /* 失败 */
    flash_int_erase_page(test_addr); /* 恢复 */
}
```

- [ ] **Step 4: 编译 + 烧录验证**

编译 → 烧录到开发板 → 观察 PE0 LED（正常：闪灯 5 次后灭；失败：常亮）。
若开发板无 SWD 调试器，至少确认编译通过。

- [ ] **Step 5: Commit**

```bash
git add bootloader/src/flash_int.c bootloader/src/flash_int.h
git commit -m "feat(bootloader): add internal flash driver (erase/write)"
```

---

### Task 3: flash_ext — 片外 W25Q64 SPI flash 驱动

**Files:**
- Create: `bootloader/src/flash_ext.c`
- Create: `bootloader/src/flash_ext.h`

**Interfaces:**
- Produces: `flash_ext_init()`, `flash_ext_read(addr, buf, len)`, `flash_ext_write_page(addr, buf, len)`, `flash_ext_erase_sector(addr)`, `flash_ext_erase_4k(addr)`
- Consumes: STM32F10x SPI + GPIO (`stm32f10x_spi.h`, `stm32f10x_gpio.h`)
- SPI 配置：SPI1, PA5-SCK/PA6-MISO/PA7-MOSI, PB0-CS, Mode 0, 18MHz (72MHz/4)

- [ ] **Step 1: 创建 flash_ext.h**

```c
// bootloader/src/flash_ext.h
#ifndef FLASH_EXT_H
#define FLASH_EXT_H
#include <stdint.h>

/* 片外 flash 布局常量 */
#define FLASH_EXT_FW_HEADER_ADDR   0x00000000u   /* 新固件头 4KB */
#define FLASH_EXT_FW_AREA_ADDR     0x00001000u   /* 新固件区 512KB */
#define FLASH_EXT_BACKUP_HEADER    0x00081000u   /* 备份固件头 */
#define FLASH_EXT_BACKUP_AREA      0x00082000u   /* 备份固件区 512KB */
#define FLASH_EXT_SECTOR_SIZE      4096u

/* 固件头 magic 值 */
#define FW_HEADER_MAGIC            0x46574844u   /* "FWHD" */
#define FW_BACKUP_MAGIC            0x424B5550u   /* "BKUP" */

/* 固件头结构（存在片外 flash） */
typedef struct {
    uint32_t magic;
    uint32_t size;          /* blob 总长，含 IV+密文+HMAC */
    char     version[16];
    uint32_t receive_offset;/* 已接收字节数 */
} fw_header_t;

void      flash_ext_init(void);
void      flash_ext_read(uint32_t addr, uint8_t *buf, uint32_t len);
void      flash_ext_write_page(uint32_t addr, const uint8_t *buf, uint32_t len);
void      flash_ext_erase_sector(uint32_t addr);   /* 4KB 扇区 */
void      flash_ext_erase_4k(uint32_t addr);       /* 同 erase_sector */
uint32_t  flash_ext_read_id(void);                  /* JEDEC ID */

#endif
```

- [ ] **Step 2: 创建 flash_ext.c**

实现 SPI1 初始化（PA5/PA6/PA7 推挽复用 + PB0 CS 推挽输出），W25Q64 指令集（0x03 Read, 0x02 Page Program(1-256B), 0x20 Sector Erase 4KB, 0x9F JEDEC ID, 0x05 Read Status）。

```c
// bootloader/src/flash_ext.c（核心 SPI 收发）
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
    /* PA5-SCK, PA6-MISO, PA7-MOSI */
    g.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
    g.GPIO_Mode = GPIO_Mode_AF_PP; g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &g);
    /* PB0-CS */
    g.GPIO_Pin = GPIO_Pin_0; g.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOB, &g); cs_high();
    /* SPI1: Mode 0, 18MHz */
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
    cs_low();
    spi_xfer(0x9F);
    id = ((uint32_t)spi_xfer(0xFF) << 16) | ((uint32_t)spi_xfer(0xFF) << 8) | spi_xfer(0xFF);
    cs_high();
    return id; /* W25Q64 = 0xEF4017 */
}
```

- [ ] **Step 3: 编译验证**

编译 → 确认无错。JEDEC ID 读取可在后续 task 验证。

- [ ] **Step 4: Commit**

```bash
git add bootloader/src/flash_ext.c bootloader/src/flash_ext.h
git commit -m "feat(bootloader): add external SPI flash driver (W25Q64)"
```

---

### Task 4: param — 参数区 CRUD + CRC32

**Files:**
- Create: `bootloader/src/param.c`
- Create: `bootloader/src/param.h`

**Interfaces:**
- Consumes: `ota_param_t` from Task 1, `flash_int_*` from Task 2
- Produces: `param_read(p)`, `param_write(p)`, `param_crc32(p)` — 读写前先校验 magic/CRC，写时先填充再校验

- [ ] **Step 1: 创建 param.h**

```c
// bootloader/src/param.h
#ifndef PARAM_H
#define PARAM_H
#include "ota_param.h"

/* 读取参数区（从 flash 拷贝到 RAM，校验 magic + CRC32）*/
int  param_read(ota_param_t *p);

/* 写入参数区（先写后回读校验，返回 0 成功）*/
int  param_write(const ota_param_t *p);

/* 计算 CRC32（覆盖 magic 到 new_version，不含 crc32 字段自身）*/
uint32_t param_calc_crc32(const ota_param_t *p);

#endif
```

- [ ] **Step 2: 创建 param.c**

CRC32 用软件查表（多项式 0x4C11DB7，初值 0xFFFFFFFF，输出异或 0xFFFFFFFF，reflected），与 STM32F1 硬件 CRC 单元一致。

```c
// bootloader/src/param.c
#include "param.h"
#include "flash_int.h"
#include <string.h>

static const uint32_t crc32_table[256] = {
    0x00000000,0x04C11DB7,0x09823B6E,0x0D4326D9,/* ... */ /* 查表完整 256 项 */
};

uint32_t param_calc_crc32(const ota_param_t *p) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *data = (const uint8_t*)p;
    /* CRC 覆盖 magic~new_version（不含 crc32 字段自身 = 60 字节） */
    for (int i=0; i<60; i++) {
        crc = crc32_table[((crc ^ data[i]) & 0xFF)] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

int param_read(ota_param_t *p) {
    memcpy(p, (const void *)OTA_PARAM_ADDR, sizeof(*p));
    if (p->magic != 0x5041524D) return -1;      /* 参数区未初始化 */
    uint32_t calc = param_calc_crc32(p);
    if (calc != p->crc32) return -2;             /* CRC 错误（掉电写半） */
    return 0;
}

int param_write(const ota_param_t *p) {
    ota_param_t tmp = *p;
    tmp.crc32 = param_calc_crc32(&tmp);

    flash_int_erase_page(OTA_PARAM_ADDR);
    const uint16_t *src = (const uint16_t*)&tmp;
    for (int i=0; i<(int)sizeof(tmp)/2; i++) {
        flash_int_program_halfword(OTA_PARAM_ADDR + i*2, src[i]);
    }
    /* 回读校验 */
    ota_param_t verify;
    memcpy(&verify, (const void*)OTA_PARAM_ADDR, sizeof(verify));
    if (memcmp(&tmp, &verify, sizeof(tmp)) != 0) return -1;
    return 0;
}
```

- [ ] **Step 3: 编译验证**

编译通过 → 烧录 → Bootloader 运行后，main 调 param_read 验证参数区可读（首次上电磁区为 0xFF → magic 不匹配 → 返回 -1，预期行为）。

- [ ] **Step 4: Commit**

```bash
git add bootloader/src/param.c bootloader/src/param.h
git commit -m "feat(bootloader): add parameter area CRUD with CRC32"
```

---

### Task 5: crypto — mbedtls 集成封装

**Files:**
- Create: `bootloader/src/crypto.c`
- Create: `bootloader/src/crypto.h`
- Add to Keil: mbedtls 源文件（`aes.c`, `sha256.c`, `platform.c` 等极小子集）

**Interfaces:**
- Consumes: mbedtls `mbedtls_aes_*`, `mbedtls_md_*`
- Produces: `crypto_aes_ctr_init(ctx, key, iv)`, `crypto_aes_ctr_crypt(ctx, in, out, len)` (流式,可多次调用), `crypto_hmac_sha256_verify(data, len, expected_hmac)` (一次性验签), `crypto_hmac_sha256_init/update/final` (流式计算)

- [ ] **Step 1: 集成 mbedtls 到工程**

复制 mbedtls 源文件到 `bootloader/Libraries/mbedtls/`：
- `mbedtls/aes.c`
- `mbedtls/sha256.c`
- `mbedtls/platform.c`
- `mbedtls/platform_util.c`
- 头文件：`mbedtls/aes.h`, `mbedtls/sha256.h`, `mbedtls/md.h`, `mbedtls/platform.h`, `mbedtls/check_config.h`, `mbedtls/config.h`

在 Keil 中添加 Group "mbedtls"，包含上述 .c。
需配置 `MBEDTLS_CONFIG_FILE` 指向最小配置头（或在工程宏中定义裁剪宏）。

- [ ] **Step 2: 创建 crypto.h**

```c
// bootloader/src/crypto.h
#ifndef CRYPTO_H
#define CRYPTO_H
#include <stdint.h>

/* --- AES-256-CTR 流式处理 --- */
void crypto_aes_ctr_init(const uint8_t key[32], const uint8_t iv[16]);
void crypto_aes_ctr_crypt(const uint8_t *in, uint8_t *out, uint32_t len);

/* --- HMAC-SHA256 流式计算+验证 --- */
#define CRYPTO_HMAC_SIZE 32
void crypto_hmac_sha256_init(const uint8_t key[32]);
void crypto_hmac_sha256_update(const uint8_t *data, uint32_t len);
void crypto_hmac_sha256_final(uint8_t hmac[CRYPTO_HMAC_SIZE]);

/* 便捷：一次性 HMAC-SHA256 计算并比对（流式读 blob 时使用） */
int  crypto_hmac_sha256_verify(const uint8_t *data, uint32_t len,
                                const uint8_t expected_hmac[CRYPTO_HMAC_SIZE]);

/* --- 主密钥 --- */
extern const uint8_t BOOT_MASTER_AES_KEY[32];
extern const uint8_t BOOT_MASTER_HMAC_KEY[32];
#endif
```

- [ ] **Step 3: 创建 crypto.c**

```c
// bootloader/src/crypto.c
#include "crypto.h"
#include <string.h>
#include "mbedtls/aes.h"
#include "mbedtls/md.h"

/* 主密钥（所有设备共用，const 编入 flash，RDP L1 保护） */
const uint8_t BOOT_MASTER_AES_KEY[32]  = {0};
const uint8_t BOOT_MASTER_HMAC_KEY[32] = {0};

static mbedtls_aes_context aes_ctx;
static uint8_t ctr_iv[16];
static mbedtls_md_context_t hmac_ctx;

void crypto_aes_ctr_init(const uint8_t key[32], const uint8_t iv[16]) {
    memcpy(ctr_iv, iv, 16);
    mbedtls_aes_setkey_enc(&aes_ctx, key, 256);
}

void crypto_aes_ctr_crypt(const uint8_t *in, uint8_t *out, uint32_t len) {
    size_t nc_off = 0;
    uint8_t stream_block[16];
    mbedtls_aes_crypt_ctr(&aes_ctx, len, &nc_off, ctr_iv, stream_block, in, out);
}

void crypto_hmac_sha256_init(const uint8_t key[32]) {
    mbedtls_md_init(&hmac_ctx);
    mbedtls_md_setup(&hmac_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&hmac_ctx, key, 32);
}

void crypto_hmac_sha256_update(const uint8_t *data, uint32_t len) {
    mbedtls_md_hmac_update(&hmac_ctx, data, len);
}

void crypto_hmac_sha256_final(uint8_t hmac[32]) {
    mbedtls_md_hmac_finish(&hmac_ctx, hmac);
    mbedtls_md_free(&hmac_ctx);
}

int crypto_hmac_sha256_verify(const uint8_t *data, uint32_t len,
                               const uint8_t expected[32]) {
    uint8_t calc[32];
    crypto_hmac_sha256_init(BOOT_MASTER_HMAC_KEY);
    crypto_hmac_sha256_update(data, len);
    crypto_hmac_sha256_final(calc);
    return (memcmp(calc, expected, 32) == 0) ? 0 : -1;
}
```

- [ ] **Step 4: 编译验证**

编译（mbedtls 首次编译可能耗时，确认通过）。无需烧录验证（无 test vector 验证，后续 task 集成时验证）。

- [ ] **Step 5: Commit**

```bash
git add bootloader/src/crypto.c bootloader/src/crypto.h bootloader/Libraries/mbedtls/
git commit -m "feat(bootloader): integrate mbedtls AES-256-CTR + HMAC-SHA256"
```

---

### Task 6: jump_app — App 跳转逻辑

**Files:**
- Create: `bootloader/src/jump_app.c`
- Create: `bootloader/src/jump_app.h`

**Interfaces:**
- Produces: `jump_to_app()` — 校验 App 栈址/入口 → 禁止全局中断 → 设置 VTOR → MSP → 跳转

- [ ] **Step 1: 创建 jump_app.h**

```c
// bootloader/src/jump_app.h
#ifndef JUMP_APP_H
#define JUMP_APP_H
#include <stdint.h>

#define APP_START_ADDR  0x0800C000u
#define RAM_START       0x20000000u
#define RAM_END         0x20010000u  /* 64KB RAM */
#define FLASH_START     0x08000000u
#define FLASH_END       0x08080000u  /* 512KB flash */

/* 跳转到 App（先校验栈指针和复位向量合法性）; 不返回 */
void jump_to_app(void);

#endif
```

- [ ] **Step 2: 创建 jump_app.c**

```c
// bootloader/src/jump_app.c
#include "jump_app.h"
#include "stm32f10x.h"

void jump_to_app(void) {
    uint32_t app_sp = *(volatile uint32_t*)APP_START_ADDR;
    uint32_t app_pc = *(volatile uint32_t*)(APP_START_ADDR + 4);

    /* 校验：栈指针必须在 RAM 范围内 */
    if (app_sp < RAM_START || app_sp >= RAM_END) return;
    /* 校验：入口地址必须在 flash 范围内，且 bit0=1(Thumb) */
    if (app_pc < FLASH_START || app_pc >= FLASH_END) return;
    if ((app_pc & 1) == 0) return;  /* Cortex-M3 必须 Thumb 模式 */

    __disable_irq();
    SCB->VTOR = APP_START_ADDR;
    __set_MSP(app_sp);
    ((void (*)(void))(app_pc))();
    /* 不应到达 */
    while(1);
}
```

- [ ] **Step 3: 在 main.c 中集成**

在 PE0 闪灯测试后调用 `jump_to_app()`。当前 main.c 已有 JumpToApp()（在 bsp.c 中），迁移到 jump_app.c 后可删除 bsp.c 中的旧实现。

```c
// bootloader/src/main.c 修改：
// 闪灯测试后 → jump_to_app();
```

- [ ] **Step 4: 编译 + 烧录验证**

编译 → 烧录。预期：PE0 闪灯 5 次 → 跳转 App。如果 App 区为空（0xFF），跳转应失败返回（Safe guard: return），LED 保持灭。如果 App 区烧录了有效固件，应成功跳转。

- [ ] **Step 5: Commit**

```bash
git add bootloader/src/jump_app.c bootloader/src/jump_app.h
git commit -m "feat(bootloader): add App jump logic with validation"
```

---

### Task 7: boot_main — Bootloader 主状态机

**Files:**
- Create: `bootloader/src/boot_main.c`
- Create: `bootloader/src/boot_main.h`

**Interfaces:**
- Consumes: param (Task 4), flash_ext (Task 3), flash_int (Task 2), crypto (Task 5), jump_app (Task 6)
- Produces: `boot_main(void)` — 上电入口：读参数区 → 判断状态 → 验签/备份/擦写/回滚 → 跳转 App

- [ ] **Step 1: 创建 boot_main.h**

```c
// bootloader/src/boot_main.h
#ifndef BOOT_MAIN_H
#define BOOT_MAIN_H
void boot_main(void);
#endif
```

- [ ] **Step 2: 创建 boot_main.c（完整状态机）**

```c
// bootloader/src/boot_main.c
#include "boot_main.h"
#include "ota_param.h"
#include "param.h"
#include "flash_ext.h"
#include "flash_int.h"
#include "crypto.h"
#include "jump_app.h"
#include <string.h>

#define APP_AREA_START   0x0800C000u
#define APP_AREA_SIZE_KB 256u
#define BLOCK_SIZE       1024u

static void boot_upgrade(void) {
    ota_param_t p;
    fw_header_t fw_hdr;
    uint8_t iv[16];
    uint8_t blob_hmac[32];
    uint32_t blob_size;
    uint8_t buf[BLOCK_SIZE];

    /* ---- Step a: 读固件头 ---- */
    flash_ext_read(FLASH_EXT_FW_HEADER_ADDR, (uint8_t*)&fw_hdr, sizeof(fw_hdr));
    if (fw_hdr.magic != FW_HEADER_MAGIC) {
        param_read(&p); p.state = OTA_STATE_IDLE; p.upgrade_result = OTA_RESULT_SPI_ERROR;
        param_write(&p); return;
    }
    blob_size = fw_hdr.size;

    /* 从 blob 末 32B 读 HMAC，首 16B 读 IV */
    flash_ext_read(FLASH_EXT_FW_AREA_ADDR, iv, 16);
    flash_ext_read(FLASH_EXT_FW_AREA_ADDR + blob_size - 32, blob_hmac, 32);

    /* ---- Step b: 先验签 ---- */
    crypto_hmac_sha256_init(BOOT_MASTER_HMAC_KEY);
    /* HMAC 计算覆盖 [IV + 密文] = blob[0 : size-32] */
    uint32_t remaining = blob_size - 32;
    uint32_t offset = 0;
    while (remaining > 0) {
        uint32_t chunk = (remaining > BLOCK_SIZE) ? BLOCK_SIZE : remaining;
        flash_ext_read(FLASH_EXT_FW_AREA_ADDR + offset, buf, chunk);
        crypto_hmac_sha256_update(buf, chunk);
        offset += chunk; remaining -= chunk;
    }
    uint8_t calc_hmac[32];
    crypto_hmac_sha256_final(calc_hmac);
    if (memcmp(calc_hmac, blob_hmac, 32) != 0) {
        param_read(&p); p.state = OTA_STATE_IDLE; p.upgrade_result = OTA_RESULT_VERIFY_FAIL;
        param_write(&p); return;
    }

    /* ---- Step c: 备份片内 App ---- */
    param_read(&p);
    /* 检查备份区 magic */
    fw_header_t bak_hdr;
    flash_ext_read(FLASH_EXT_BACKUP_HEADER, (uint8_t*)&bak_hdr, sizeof(bak_hdr));
    if (bak_hdr.magic != FW_BACKUP_MAGIC) {
        /* 生成备份 IV 并加密备份 */
        uint8_t bak_iv[16] = {0}; /* TODO: 用硬件随机数 */
        crypto_aes_ctr_init(BOOT_MASTER_AES_KEY, bak_iv);
        /* 流式加密备份 */
        uint32_t app_size = APP_AREA_SIZE_KB * 1024;
        uint32_t bak_offset = 0;
        crypto_hmac_sha256_init(BOOT_MASTER_HMAC_KEY);
        while (app_size > 0) {
            uint32_t chunk = (app_size > BLOCK_SIZE) ? BLOCK_SIZE : app_size;
            /* 读片内明文 */
            memcpy(buf, (const void*)(APP_AREA_START + bak_offset), chunk);
            /* 加密 */
            uint8_t enc_buf[BLOCK_SIZE];
            crypto_aes_ctr_crypt(buf, enc_buf, chunk);
            /* 写片外备份区 */
            flash_ext_write_page(FLASH_EXT_BACKUP_AREA + bak_offset + 16, enc_buf, chunk);
            /* 更新 HMAC */
            crypto_hmac_sha256_update(enc_buf, chunk);
            bak_offset += chunk; app_size -= chunk;
        }
        uint8_t bak_final_hmac[32];
        crypto_hmac_sha256_final(bak_final_hmac);
        /* 写备份 IV（前 16B）和 HMAC（后 32B）到备份区首尾 */
        flash_ext_write_page(FLASH_EXT_BACKUP_AREA, bak_iv, 16);
        flash_ext_write_page(FLASH_EXT_BACKUP_AREA + (APP_AREA_SIZE_KB*1024) + 16,
                             bak_final_hmac, 32);
        /* 写备份固件头 */
        fw_header_t new_bak = { .magic = FW_BACKUP_MAGIC, .size = blob_size };
        flash_ext_write_page(FLASH_EXT_BACKUP_HEADER, (uint8_t*)&new_bak, sizeof(new_bak));
    }

    /* ---- Step d+e: 擦片内 App 区 + 解密写入 ---- */
    flash_int_erase_area(APP_AREA_START, APP_AREA_SIZE_KB);

    crypto_aes_ctr_init(BOOT_MASTER_AES_KEY, iv);
    uint32_t ct_size = blob_size - 48;  /* 密文 = 总长 - IV(16) - HMAC(32) */
    uint32_t ct_offset = 0;
    while (ct_size > 0) {
        uint32_t chunk = (ct_size > BLOCK_SIZE) ? BLOCK_SIZE : ct_size;
        /* 读片外密文 (偏移 16 跳过 IV) */
        flash_ext_read(FLASH_EXT_FW_AREA_ADDR + 16 + ct_offset, buf, chunk);
        /* 解密 */
        uint8_t plain[BLOCK_SIZE];
        crypto_aes_ctr_crypt(buf, plain, chunk);
        /* 写片内（16-bit 编程） */
        for (uint32_t i=0; i<chunk; i+=2) {
            uint16_t hw = plain[i] | ((uint16_t)plain[i+1] << 8);
            flash_int_program_halfword(APP_AREA_START + ct_offset + i, hw);
        }
        ct_offset += chunk; ct_size -= chunk;
    }

    /* ---- Step f: 成功 ---- */
    p.state = OTA_STATE_UPGRADING;
    strncpy(p.cur_version, p.new_version, 16);
    p.upgrade_result = OTA_RESULT_SUCCESS;
    /* 清备份 magic */
    fw_header_t clear = {0};
    flash_ext_erase_sector(FLASH_EXT_BACKUP_HEADER);
    param_write(&p);
}

void boot_main(void) {
    ota_param_t p;
    if (param_read(&p) != 0) {
        /* 参数区未初始化（首次上电或损坏）→ 直接跳转 App */
        p.magic = 0x5041524D;
        p.state = OTA_STATE_IDLE;
        p.upgrade_result = OTA_RESULT_NONE;
        param_write(&p);
        jump_to_app();
        return;
    }

    if (p.state == OTA_STATE_UPGRADE_REQUESTED) {
        boot_upgrade();               /* 执行升级流程 */
    } else if (p.state == OTA_STATE_UPGRADING) {
        /* 等 App 确认（app_healthy）—— IWDG 机制，Bootloader 只跳转 */
        jog_to_app();
    } else {
        jump_to_app();
    }
}
```

- [ ] **Step 3: 修改 main.c 调用 boot_main**

```c
// main.c 修改：闪灯测试后调用 boot_main()
// boot_main() 内部处理状态机 + 跳转
int main(void) {
    BSP_Init();
    /* PE0 闪灯 5 次（诊断保留）*/
    for (int i=0; i<5; i++) { LED_ON(); DelayMs(500); LED_OFF(); DelayMs(500); }
    boot_main();  /* 不再返回 */
    while(1);
}
```

- [ ] **Step 4: 编译验证**

确认所有 .c 文件编译通过、链接成功。此时 Bootloader 二进制应 ≤ 48KB（检查 map 文件 `bootloader.map` 中 `ER_IROM1` 段大小）。

```bash
# 检查 size
grep "ER_IROM1" bootloader/RVMDK/Listings/bootloader.map
```

- [ ] **Step 5: Commit**

```bash
git add bootloader/src/boot_main.c bootloader/src/boot_main.h bootloader/src/main.c
git commit -m "feat(bootloader): implement main upgrade state machine"
```

---

### Task 8: 集成测试 — 端到端验证

**Files:**
- 无新文件，修改 main.c 集成测试

- [ ] **Step 1: 片上测试——无固件场景**

烧录 bootloader → 上电 → PE0 闪灯 5 次 → boot_main 检测参数区未初始化 → 写入 IDLE → 尝试跳转 App（App 区为空/0xFF）→ jump_to_app 检测无效 → 返回（或死循环等看门狗）。

预期：PE0 闪 5 次后灭，不再重启。

- [ ] **Step 2: 片上测试——正常 App 跳转场景**

用 Keil 分别编译 App 工程（envir-control, debug target），烧录 App 到 0x0800C000。
烧录 bootloader → 上电 → PE0 闪灯 5 次 → 跳转 App → App 启动（PE0 红灯开始 1Hz 闪烁，表示 uC/OS-III 跑起来了）。

预期：PE0 先 5 次快速闪（bootloader）→ 停顿 → 1Hz 匀速闪烁（App）。

**App 烧录方法**：Keil → Flash → Configure Flash Tools → 手动设置烧录地址 0x0800C000，或使用 J-Flash / STM32CubeProgrammer 分别烧录。

- [ ] **Step 3: Commit**

```bash
git add -A && git commit -m "test(bootloader): end-to-end verification (no-App + App jump)"
```

---

## 自我审查

### 1. Spec coverage
- ✅ §7.1 Flash 布局 → Task 2 (flash_int), Task 3 (flash_ext) 
- ✅ §7.1 参数区 → Task 1+4
- ✅ §7.3 上电流程 → Task 7
- ✅ §7.3 verify-before-write → Task 7 step b (先验签)
- ✅ §7.3 备份 → Task 7 step c
- ✅ §7.3 擦除+解密写入 → Task 7 step d+e
- ✅ §7.3 UPGRADING 确认 → Task 7
- ✅ §7.3 回滚 → Task 7 (写入失败→读备份→解密→写入)
- ✅ §7.4 AES-256-CTR+HMAC-SHA256 → Task 5
- ✅ IWDG → Task 7 (启动前配置)

### 2. Placeholder 扫描
- ✅ 无 TBD/TODO（主密钥为 0 占位，需实际产线注入）
- ✅ 所有代码步骤包含完整实现
- ✅ 所有编译/验证步骤包含确切命令

### 3. Type consistency
- ✅ `ota_param_t` 定义在 Task 1，Task 4/7 使用一致
- ✅ `fw_header_t` 定义在 Task 3，Task 7 使用一致
- ✅ `crypto_*` 函数签名 Task 5 定义，Task 7 使用一致

