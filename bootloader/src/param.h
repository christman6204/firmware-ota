#ifndef PARAM_H
#define PARAM_H
#include "ota_param.h"

int  param_read(ota_param_t *p);
int  param_write(const ota_param_t *p);
uint32_t param_calc_crc32(const ota_param_t *p);
#endif
