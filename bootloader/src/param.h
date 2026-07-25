/**
 * @file    param.h
 * @brief   OTA 参数管理接口
 *
 * @details OTA 参数 (ota_param_t) 存储在片内 Flash 的指定页中，
 *          包含固件版本、URL、端口、CRC32 校验等信息。
 *
 *          本模块提供参数读写与 CRC32 校验功能，
 *          写入时自动计算并填充 CRC32 字段，读取时自动校验。
 *
 * @see     ota_param_t 定义在 ota_param.h 中
 */

#ifndef PARAM_H
#define PARAM_H

#include "ota_param.h"

/**
 * @brief   从片内 Flash 读取 OTA 参数并校验
 *
 *          校验顺序:
 *          1. Magic 校验: 检查 magic 是否为 0x5041524D ("PARM")
 *          2. CRC32 校验: 对结构体前 60 字节计算 CRC32，与 crc32 字段比较
 *
 * @param   p  输出参数，指向 ota_param_t 结构体用于接收读取结果
 * @retval   0  读取成功且校验通过
 * @retval  -1  Magic 校验失败 (参数区未初始化或损坏)
 * @retval  -2  CRC32 校验失败 (参数数据损坏)
 */
int  param_read(ota_param_t *p);

/**
 * @brief   将 OTA 参数写入片内 Flash
 *
 *          写入流程:
 *          1. 复制参数到临时变量，计算并填充 CRC32 字段
 *          2. 擦除 OTA_PARAM_ADDR 所在的页 (2 KB)
 *          3. 逐半字 (16-bit) 编程写入
 *          4. 回读验证: 从 Flash 读回并与写入内容做 memcmp
 *
 * @param   p  指向要写入的 ota_param_t (crc32 字段由函数内部自动填充，传入值被忽略)
 * @retval   0  写入成功且验证通过
 * @retval  -1  写入后验证失败 (Flash 编程异常)
 */
int  param_write(const ota_param_t *p);

/**
 * @brief   计算 OTA 参数的 CRC32 校验值
 *
 *          CRC32 参数:
 *          - 多项式 (poly):   0x4C11DB7
 *          - 输入反射 (refin):   true
 *          - 输出反射 (refout):  true
 *          - 初始值 (init):  0xFFFFFFFF
 *          - 最终异或 (xorout):  0xFFFFFFFF
 *
 * @attention  仅对结构体的前 60 字节计算 CRC32，
 *             不包含 crc32 字段自身 (避免循环依赖)。
 *
 * @param   p  指向要计算 CRC32 的 ota_param_t
 * @return  计算得到的 CRC32 值
 */
uint32_t param_calc_crc32(const ota_param_t *p);

#endif
