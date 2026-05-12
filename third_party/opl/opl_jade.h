#ifndef OPL_JADE_H
#define OPL_JADE_H

#include <inttypes.h>
#include <stdint.h>

#include "opl_internal.h"

extern opl_driver_t opl_jade_driver;

void OPL_Jade_Render_Add(int16_t *buffer, unsigned int buffer_len);
void OPL_Jade_DelayUs(uint64_t us);

#endif
