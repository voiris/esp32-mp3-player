#include "utils.h"

#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "local_drivers/ssd1306.h"

char *concat_path(const char *path1, const char *path2) {
    const size_t len1 = strlen(path1);
    const size_t len2 = strlen(path2);

    const bool has_trailing = (len1 > 0 && path1[len1 - 1] == '/');
    const bool has_leading  = (len2 > 0 && path2[0] == '/');

    int slash_delta = 0;
    if (!has_trailing && !has_leading)  slash_delta = +1;
    if ( has_trailing &&  has_leading)  slash_delta = -1;

    const size_t total = len1 + len2 + slash_delta + 1;

    char *result = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!result) {
        ESP_LOGE("UTILS", "concat_path: malloc failed (%zu bytes)", total);
        abort();
    }

    const size_t copy1 = (slash_delta == -1) ? len1 - 1 : len1;
    memcpy(result, path1, copy1);
    result[copy1] = '\0';

    if (slash_delta == +1) strcat(result, "/");

    strcat(result, (slash_delta == -1) ? path2 + 1 : path2);

    return result;
}

void display_init(ssd1306_t* display) {
    s_display = display;
}

void display_first_line(const char *text) {
    ssd1306_clear(s_display);
    ssd1306_draw_string(s_display, 0, 0, text, true);
    ssd1306_refresh(s_display);
}
