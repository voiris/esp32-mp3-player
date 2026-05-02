#include "esp_log.h"

#include "mp3_decoder.h"
#include "utils.h"
#include "local_drivers/ir_rx.h"
#include "local_drivers/ssd1306.h"

#define LOG_TAG          "PLAYER"

#define PLAY_STACK_SIZE  4096
#define PLAY_PRIORITY    4

#define MAX_VOLUME 1.0f
#define MIN_VOLUME 0.0f

static char     **s_playlist     = NULL;
static size_t     s_playlist_len = 0;

static volatile size_t s_next_track_idx = 0;

static TaskHandle_t s_play_task_handle = NULL;

static float volume = CONFIG_START_VOLUME * 0.01f * 0.1f;
static const float VOLUME_STEP = CONFIG_VOLUME_STEP * 0.01f;



static void ir_callback(const uint32_t code) {
    ESP_LOGI(LOG_TAG, "IR received: 0x%08" PRIX32, code);

    char str[12];
    snprintf(str, sizeof(str), "0x%08X", (unsigned)code);
    display_first_line(str);

    switch (code) {
        case CONFIG_IR_NEXT_TRACK:
            system_mute();
            mp3_decoder_stop();
            break;

        case CONFIG_IR_PREV_TRACK:
            system_mute();
            s_next_track_idx =
                    (s_next_track_idx + s_playlist_len - 2) % s_playlist_len;
            mp3_decoder_stop();
            break;

        case CONFIG_IR_PAUSE:
            user_mute_switch();
            break;

        case CONFIG_IR_VOLUME_UP:
            volume += VOLUME_STEP;
            if (volume > MAX_VOLUME) {
                volume = MAX_VOLUME;
            }
            update_volume(volume);

        case CONFIG_IR_VOLUME_DOWN:
            volume -= VOLUME_STEP;
            if (volume < MIN_VOLUME) {
                volume = MIN_VOLUME;
            }
            update_volume(volume);

        default:
            break;
    }
}

void player_task(void *_) {
    while (true) {
        const size_t idx = s_next_track_idx % s_playlist_len;

        char buf[32];
        snprintf(buf, sizeof(buf), "Track %zu/%zu", idx + 1, s_playlist_len);
        display_first_line(buf);

        ESP_LOGI(LOG_TAG, "Playing [%zu]: %s", idx, s_playlist[idx]);
        mp3_decode_file(s_playlist[idx]);

        s_next_track_idx = (s_next_track_idx + 1) % s_playlist_len;
    }
}

void player_init(char **playlist, const size_t playlist_len) {
    s_playlist     = playlist;
    s_playlist_len = playlist_len;

    gpio_set_direction(CONFIG_MUTE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_MUTE_PIN, false);

    ESP_LOGI(LOG_TAG, "Playlist ready: %zu tracks", s_playlist_len);

    set_ir_callback(ir_callback);
    ir_rx_task_init();

    xTaskCreate(player_task, "play_task", PLAY_STACK_SIZE,
                NULL, PLAY_PRIORITY, &s_play_task_handle);
}
