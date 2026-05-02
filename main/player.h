#pragma once

#include "local_drivers/ssd1306.h"

void player_task(void *pvParameters);

void player_init(char **playlist, size_t playlist_len);
