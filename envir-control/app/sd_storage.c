/**
 * @file    sd_storage.c
 * @brief   SD 卡本地数据存储管理实现
 *
 * @details
 * ## 数据流
 *   采集任务(每秒) -> sd_storage_write() -> write_buf[4KB]
 *                                              │
 *                               缓冲满(~40秒) -> f_write() -> SD 卡
 *                               日期变化       -> 切换文件
 *
 * ## 文件管理
 *   /DATA/YYYYMMDD.DAT  (8.3 格式, 每天一个文件)
 *   启动时 + 每天午夜: 删除 > 90 天的文件
 *
 * @author Christman
 * @date   2026
 */

#include "sd_storage.h"
#include "ff.h"
#include "diskio.h"
#include "os.h"
#include <string.h>

/* ============================ 静态变量 ============================ */

/* FatFs 对象 (静态分配, 避免使用堆) */
static FATFS  fs;                    /* 文件系统对象 */
static FIL    data_file;             /* 当前数据文件 */
static uint8_t file_open  = 0;       /* 文件是否已打开 */

/* 写入缓冲区 */
static uint8_t  write_buf[SD_WRITE_BUF_SIZE];
static uint16_t buf_offset = 0;

/* 当前日期 (YYYYMMDD, 用于检测日期变化) */
static uint32_t current_yyyymmdd = 0;

/* ============================ 时间工具函数 ============================ */

/** 检查闰年 */
static uint8_t is_leap_year(uint16_t year) {
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

/** 月份天数表 */
static const uint8_t days_in_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/**
 * @brief  计算从 1970-01-01 到指定日期的天数
 * @return 天数 (用于日期差比较)
 */
static uint32_t date_to_days(uint16_t year, uint8_t month, uint8_t day) {
    uint32_t days = 0;
    uint16_t y;
    uint8_t  m;

    for (y = 1970; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    for (m = 1; m < month; m++) {
        days += days_in_month[m - 1];
        if (m == 2 && is_leap_year(year)) days += 1;
    }
    days += day - 1;
    return days;
}

/**
 * @brief  FatFs 时间戳函数 (FF_FS_NORTC=0 时必需)
 * @return FAT 格式时间戳 (DWORD)
 *
 * 位域: [31:25]年-1980 [24:21]月 [20:16]日 [15:11]时 [10:5]分 [4:0]秒/2
 */
DWORD get_fattime(void) {
    rtc_time_t t;
    rtc_get_time(&t);
    return ((DWORD)(t.year - 1980) << 25)
         | ((DWORD)t.month << 21)
         | ((DWORD)t.day << 16)
         | ((DWORD)t.hour << 11)
         | ((DWORD)t.minute << 5)
         | ((DWORD)(t.second / 2));
}

/**
 * @brief  生成 YYYYMMDD 整数 (用于文件名和日期比较)
 */
static uint32_t make_yyyymmdd(uint16_t y, uint8_t m, uint8_t d) {
    return (uint32_t)y * 10000u + (uint32_t)m * 100u + d;
}

/* ============================ 文件名工具 ============================ */

/**
 * @brief  生成文件路径: /DATA/YYYYMMDD.DAT
 * @param  path      输出缓冲区 (至少 18 字节)
 * @param  yyyymmdd  日期整数
 */
static void make_filename(char *path, uint32_t yyyymmdd) {
    /* 8.3 格式: YYYYMMDD.DAT (FatFs FF_USE_LFN=0 支持) */
    path[0] = '/'; path[1] = 'D'; path[2] = 'A'; path[3] = 'T'; path[4] = 'A'; path[5] = '/';
    path[6]  = '0' + (yyyymmdd / 10000000u) % 10;
    path[7]  = '0' + (yyyymmdd / 1000000u) % 10;
    path[8]  = '0' + (yyyymmdd / 100000u) % 10;
    path[9]  = '0' + (yyyymmdd / 10000u) % 10;
    path[10] = '0' + (yyyymmdd / 1000u) % 10;
    path[11] = '0' + (yyyymmdd / 100u) % 10;
    path[12] = '0' + (yyyymmdd / 10u) % 10;
    path[13] = '0' + (yyyymmdd / 1u) % 10;
    path[14] = '.';
    path[15] = 'D'; path[16] = 'A'; path[17] = 'T';
    path[18] = '\0';
}

/**
 * @brief  从文件名解析 YYYYMMDD (用于过期判断)
 * @param  fname  文件名 (如 "20260729.DAT")
 * @return YYYYMMDD 整数, 0 表示无效
 */
static uint32_t parse_filename(const char *fname) {
    uint32_t result = 0;
    uint8_t i;
    /* 文件名应为 8 位数字 + .DAT */
    for (i = 0; i < 8; i++) {
        if (fname[i] < '0' || fname[i] > '9') return 0;
        result = result * 10u + (fname[i] - '0');
    }
    return result;
}

/* ============================ 过期文件清理 ============================ */

/**
 * @brief  删除 /DATA 目录下超过 90 天的文件
 */
static void cleanup_old_files(void) {
    DIR dir;
    FILINFO fno;
    FRESULT fr;
    rtc_time_t now;
    uint32_t now_days;
    uint32_t file_date;
    uint32_t file_days;
    uint16_t y; uint8_t m, d;
    char path[18];

    rtc_get_time(&now);
    now_days = date_to_days(now.year, now.month, now.day);

    fr = f_opendir(&dir, "/DATA");
    if (fr != FR_OK) return;

    while (1) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == '\0') break;

        file_date = parse_filename(fno.fname);
        if (file_date == 0) continue;  /* 非数据文件, 跳过 */

        /* 分解 YYYYMMDD -> 年月日 */
        y = file_date / 10000u;
        m = (file_date / 100u) % 100u;
        d = file_date % 100u;

        file_days = date_to_days(y, m, d);
        if (now_days > file_days && (now_days - file_days) > SD_RETENTION_DAYS) {
            /* 超过保留期, 删除 */
            make_filename(path, file_date);
            f_unlink(path);
        }
    }
    f_closedir(&dir);
}

/* ============================ 文件管理 ============================ */

/**
 * @brief  打开当天数据文件 (追加模式)
 */
static int open_daily_file(uint32_t yyyymmdd) {
    FRESULT fr;
    char path[18];

    if (file_open) {
        f_close(&data_file);
        file_open = 0;
    }

    make_filename(path, yyyymmdd);
    fr = f_open(&data_file, path, FA_WRITE | FA_OPEN_ALWAYS);
    if (fr != FR_OK) return -1;

    /* 移动到文件末尾 (追加) */
    fr = f_lseek(&data_file, f_size(&data_file));
    if (fr != FR_OK) {
        f_close(&data_file);
        return -1;
    }

    file_open = 1;
    current_yyyymmdd = yyyymmdd;
    return 0;
}

/* ============================ 对外接口 ============================ */

int sd_storage_init(void) {
    FRESULT fr;
    rtc_time_t now;
    uint32_t yyyymmdd;

    /* 1. 挂载 FatFs (disk_initialize 内部会调 SD_SPI_Init 初始化 SD 卡) */
    fr = f_mount(&fs, "", 1);  /* 1 = 立即挂载 */
    if (fr != FR_OK) return -2;

    /* 3. 创建 /DATA 目录 (已存在则忽略错误) */
    f_mkdir("/DATA");

    /* 4. 清理过期文件 */
    cleanup_old_files();

    /* 5. 打开当天文件 */
    rtc_get_time(&now);
    yyyymmdd = make_yyyymmdd(now.year, now.month, now.day);
    if (open_daily_file(yyyymmdd) != 0) return -3;

    return 0;
}

int sd_storage_write(uint32_t ts, const float *data24) {
    uint8_t *p;
    uint16_t i;

    if (!file_open) return -1;

    /* 检查缓冲区剩余空间 */
    if (buf_offset + SD_REC_SIZE > SD_WRITE_BUF_SIZE) {
        if (sd_storage_flush() != 0) return -2;
    }

    /* 打包记录: timestamp + 24×float */
    p = &write_buf[buf_offset];
    p[0] = (uint8_t)(ts);
    p[1] = (uint8_t)(ts >> 8);
    p[2] = (uint8_t)(ts >> 16);
    p[3] = (uint8_t)(ts >> 24);
    for (i = 0; i < SD_REC_CHANNELS; i++) {
        uint32_t v;
        memcpy(&v, &data24[i], 4);  /* float -> uint32 (不转换, 原样存储) */
        p[4 + i*4]     = (uint8_t)(v);
        p[4 + i*4 + 1] = (uint8_t)(v >> 8);
        p[4 + i*4 + 2] = (uint8_t)(v >> 16);
        p[4 + i*4 + 3] = (uint8_t)(v >> 24);
    }

    buf_offset += SD_REC_SIZE;
    return 0;
}

int sd_storage_flush(void) {
    FRESULT fr;
    UINT bw;
    rtc_time_t now;
    uint32_t yyyymmdd;

    if (!file_open) return -1;
    if (buf_offset == 0) return 0;

    /* 检查日期是否变化 */
    rtc_get_time(&now);
    yyyymmdd = make_yyyymmdd(now.year, now.month, now.day);
    if (yyyymmdd != current_yyyymmdd) {
        /* 日期变化: 先 flush 旧文件, 打开新文件 */
        fr = f_write(&data_file, write_buf, buf_offset, &bw);
        f_close(&data_file);
        file_open = 0;
        buf_offset = 0;
        cleanup_old_files();
        if (open_daily_file(yyyymmdd) != 0) return -2;
        return 0;
    }

    /* 写入缓冲到 SD 卡 */
    fr = f_write(&data_file, write_buf, buf_offset, &bw);
    if (fr != FR_OK || bw != buf_offset) return -3;

    buf_offset = 0;
    return 0;
}

/* ============================ 数据采集桩函数 ============================ */
/* TODO: 替换为真实 ADC/传感器采集 */
static void collect_data(float *data24) {
    uint16_t i;
    for (i = 0; i < SD_REC_CHANNELS; i++) {
        data24[i] = (float)i * 1.0f;  /* 测试数据 */
    }
}

/* ============================ uC/OS-III 任务 ============================ */

void sd_storage_task(void *p_arg) {
    OS_ERR      err;
    uint32_t    ts;
    float       data24[SD_REC_CHANNELS];
    uint8_t     init_ok = 0;

    (void)p_arg;

    /* 初始化 SD 卡存储 (失败则定期重试) */
    while (!init_ok) {
        if (sd_storage_init() == 0) {
            init_ok = 1;
        } else {
            /* 初始化失败, 等 5 秒重试 */
            OSTimeDlyHMSM(0, 0, 5, 0,
                          OS_OPT_TIME_HMSM_STRICT,
                          &err);
        }
    }

    /* 主循环: 每秒采集并写入 */
    while (1) {
        /* 获取时间戳 (直接读 RTC 计数器) */
        ts = rtc_get_unix();

        /* 采集数据 (桩函数, 后续替换为真实采集) */
        collect_data(data24);

        /* 写入 SD 卡缓冲 */
        sd_storage_write(ts, data24);

        /* 每秒一次 */
        OSTimeDlyHMSM(0, 0, 1, 0,
                      OS_OPT_TIME_HMSM_STRICT,
                      &err);
    }
}
