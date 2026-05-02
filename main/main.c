#include <stdio.h>
#include <string.h>
#include <sys/dirent.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#include "local_drivers/i2s_output.h"
#include "local_drivers/sd_card_mount.h"
#include "player.h"
#include "local_drivers/ssd1306.h"
#include "utils.h"

#define LOG_TAG  "MAIN"

static ssd1306_t* s_display = NULL;

void app_main(void)
{
    s_display = heap_caps_malloc(sizeof(ssd1306_t), MALLOC_CAP_SPIRAM);

    ESP_ERROR_CHECK(ssd1306_init(
        s_display, I2C_NUM_0,
        CONFIG_SSD1306_SDA_GPIO_PIN, CONFIG_SSD1306_SCL_GPIO_PIN,
        400000));
    display_init(s_display);
    display_first_line("Loading...");

    char *playlist_path = concat_path(CONFIG_MOUNT_POINT, CONFIG_MUSIC_PATH);
    ESP_LOGI(LOG_TAG, "Playlist path: %s", playlist_path);

    mount_sd_card();

    i2s_output_init(44100);

    DIR *dir = opendir(playlist_path);
    if (!dir) {
        ESP_LOGE(LOG_TAG, "Cannot open directory: %s", playlist_path);
        display_first_line("No SD card?");
        heap_caps_free(playlist_path);
        return;
    }

    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".MP3") || strstr(entry->d_name, ".mp3"))
            count++;
    }

    if (count == 0) {
        closedir(dir);
        heap_caps_free(playlist_path);
        ESP_LOGW(LOG_TAG, "No MP3 files found in %s", playlist_path);
        display_first_line("No MP3 found");
        return;
    }

    char **mp3_files = heap_caps_malloc(count * sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!mp3_files) {
        closedir(dir);
        heap_caps_free(playlist_path);
        ESP_LOGE(LOG_TAG, "playlist array alloc failed");
        display_first_line("Out of memory");
        return;
    }

    rewinddir(dir);
    size_t idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < count) {
        if (!strstr(entry->d_name, ".MP3") && !strstr(entry->d_name, ".mp3"))
            continue;

        const size_t path_len = strlen(playlist_path) + 1
                              + strlen(entry->d_name) + 1;
        mp3_files[idx] = heap_caps_malloc(path_len, MALLOC_CAP_SPIRAM);
        if (!mp3_files[idx]) {
            ESP_LOGE(LOG_TAG, "path alloc failed for %s", entry->d_name);
            continue;
        }
        snprintf(mp3_files[idx], path_len, "%s/%s", playlist_path, entry->d_name);
        ESP_LOGI(LOG_TAG, "  [%zu] %s", idx, mp3_files[idx]);
        idx++;
    }
    closedir(dir);
    heap_caps_free(playlist_path);
    playlist_path = NULL;

    if (idx == 0) {
        ESP_LOGE(LOG_TAG, "All path allocs failed");
        heap_caps_free(mp3_files);
        display_first_line("Alloc error");
        return;
    }

    player_init(mp3_files, idx);
}