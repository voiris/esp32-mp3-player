#pragma once

#include <stdint.h>
#include <stddef.h>

void i2s_output_init(uint32_t sample_rate);
void i2s_output_write(const int16_t *pcm, size_t samples);
void i2s_output_set_sample_rate(uint32_t rate);
