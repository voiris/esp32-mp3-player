#pragma once

#include <stdint.h>

typedef void (*ir_callback_t)(uint32_t code);

void set_ir_callback(ir_callback_t callback);

void ir_rx_task_init(void);
