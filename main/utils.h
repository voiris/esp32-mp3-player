#pragma once

#include "local_drivers/ssd1306.h"

static ssd1306_t* s_display;

char *concat_path(const char *path1, const char *path2);

void display_init(ssd1306_t* display);

void display_first_line(const char *text);
