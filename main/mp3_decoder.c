#include "mp3_decoder.h"
#include "mp3dec.h"
#include "local_drivers/i2s_output.h"
#include "local_drivers/id3.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"


#define LOG_TAG          "mp3_decoder"

#define PCM_BUF_SAMPLES  (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)   /* 2304 */

#define SD_READER_STACK_SIZE 4096
#define SD_READER_PRIORITY   6
#define I2S_TASK_STACK_SIZE  4096
#define I2S_TASK_PRIORITY    7

typedef struct {
    uint8_t data[CONFIG_MP3_CHUNK_SIZE];
    int     len;
} chunk_t;

typedef struct {
    FILE        *file;
    TaskHandle_t caller;
} reader_ctx_t;

typedef struct {
    int16_t data[PCM_BUF_SAMPLES * 2];
    int     samples;
} pcm_chunk_t;

static QueueHandle_t s_pcm_queue  = NULL;
static QueueHandle_t s_pcm_free_q = NULL;

static QueueHandle_t s_chunk_queue  = NULL;
static QueueHandle_t s_free_queue   = NULL;
static TaskHandle_t  s_reader_handle = NULL;
static TaskHandle_t  s_i2s_task_handle = NULL;

static uint8_t     *s_work_buf = NULL;
static int16_t     *s_pcm_buf  = NULL;
static HMP3Decoder  s_decoder  = NULL;

static volatile bool s_stop_requested = false;
static volatile bool s_i2s_stop_requested = false;

static float s_volume = 0.0f;

static bool is_muted_by_user = false;
static bool is_muted_by_system = false;

static bool is_muted(void) { return is_muted_by_user || is_muted_by_system; };

static void update_mute(void) {
    printf("Update mute: %i\n", is_muted());
    gpio_set_level(CONFIG_MUTE_PIN, is_muted());
}

void user_mute_switch(void) {
    is_muted_by_user = !is_muted_by_user;
    update_mute();
}

void system_mute(void) {
    is_muted_by_system = true;
    update_mute();
}

void system_unmute(void) {
    is_muted_by_system = false;
    update_mute();
}

void update_volume(const float volume) {
    s_volume = volume;
}

static void skip_id3(FILE *f) {
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) < 10) { fseek(f, 0, SEEK_SET); return; }
    if (hdr[0]=='I' && hdr[1]=='D' && hdr[2]=='3') {
        const uint32_t sz = ((uint32_t)(hdr[6]&0x7F)<<21)
                    | ((uint32_t)(hdr[7]&0x7F)<<14)
                    | ((uint32_t)(hdr[8]&0x7F)<< 7)
                    |  (uint32_t)(hdr[9]&0x7F);
        ESP_LOGI(LOG_TAG, "Skip ID3v2 size=%" PRIu32, sz);
        fseek(f, (long)sz, SEEK_CUR);
    } else {
        fseek(f, 0, SEEK_SET);
    }
}

static void drain_and_free_queue(QueueHandle_t q) {
    if (!q) return;
    void *item = NULL;
    while (xQueueReceive(q, &item, 0) == pdPASS)
        heap_caps_free(item);
}

static void delete_queue(QueueHandle_t *q) {
    if (!*q) return;
    vQueueDelete(*q);
    *q = NULL;
}

static void reader_task(void *arg) {
    reader_ctx_t *ctx    = (reader_ctx_t *)arg;
    FILE         *f      = ctx->file;
    TaskHandle_t  caller = ctx->caller;

    while (!s_stop_requested) {
        chunk_t *chunk = NULL;
        if (xQueueReceive(s_free_queue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;

        chunk->len = (int)fread(chunk->data, 1, CONFIG_MP3_CHUNK_SIZE, f);
        xQueueSend(s_chunk_queue, &chunk, portMAX_DELAY);

        if (chunk->len == 0) break;
    }

    xTaskNotifyGive(caller);
    vTaskDelete(NULL);
}

static void i2s_task(void *_) {
    while (!s_i2s_stop_requested) {
        pcm_chunk_t *pcm = NULL;

        if (xQueueReceive(s_pcm_queue, &pcm, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (pcm->samples == 0) {
                xQueueSend(s_pcm_free_q, &pcm, 0);
                break;
            }

            i2s_output_write(pcm->data, pcm->samples);

            xQueueSend(s_pcm_free_q, &pcm, portMAX_DELAY);
        }
        if (ulTaskNotifyTake(pdTRUE, 0)) {
            break;
        }
    }

    vTaskDelete(NULL);
}

void mp3_decoder_stop(void)
{
    s_stop_requested = true;
}

void mp3_stop_i2s()
{
    s_i2s_stop_requested = true;

    if (s_i2s_task_handle) {
        xTaskNotifyGive(s_i2s_task_handle);
    }

    // Отправляем «пустой» PCM, чтобы гарантированно разбудить i2s_task
    pcm_chunk_t *pcm = NULL;
    if (xQueueReceive(s_pcm_free_q, &pcm, 0) == pdTRUE) {
        pcm->samples = 0;
        xQueueSend(s_pcm_queue, &pcm, 0);
    }
}

static ID3Tag* take_id3_tag(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return NULL; }

    ID3Tag *tag = heap_caps_malloc(sizeof(ID3Tag), MALLOC_CAP_SPIRAM);
    id3_tag_init(tag);
    ID3Result r = id3_parse_fp(fp, tag);
    fclose(fp);

    if (r == ID3_OK) {
        printf("\n[FILE* API] Версия: %s\n", id3_version_str(tag->version));
        if (tag->has_replay_gain)
            printf("[FILE* API] ReplayGain Track: %+.2f dB\n",
                   tag->replay_gain.track_gain_db);
    }
    return tag; // required: id3_free(tag); heap_caps_free(tag);
}

void mp3_decode_file(const char *path)
{
    system_unmute();

    s_stop_requested = false;
    s_i2s_stop_requested = false;

    ID3Tag* tag = take_id3_tag(path);
    if (!tag) {
        return;
    }

    const char* title = id3_text(&tag->title)  ?: strrchr(path, '/');

    display_first_line(title);

    printf("Title:  %s\n", title);
    printf("Artist: %s\n", id3_text(&tag->artist) ?: "(нет)");
    printf("Album:  %s\n", id3_text(&tag->album)  ?: "(нет)");

    id3_free(tag);
    heap_caps_free(tag);

    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(LOG_TAG, "Cannot open: %s", path); return; }
    skip_id3(f);

    if (!s_decoder) {
        s_decoder = MP3InitDecoder();
        if (!s_decoder) {
            ESP_LOGE(LOG_TAG, "MP3InitDecoder failed");
            fclose(f); return;
        }
    }

    if (!s_work_buf) {
        s_work_buf = heap_caps_malloc(CONFIG_MP3_CHUNK_SIZE * 2, MALLOC_CAP_SPIRAM);
        if (!s_work_buf) {
            ESP_LOGE(LOG_TAG, "work_buf alloc failed");
            fclose(f); return;
        }
    }
    if (!s_pcm_buf) {
        s_pcm_buf = heap_caps_malloc(PCM_BUF_SAMPLES * 2 * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM);
        if (!s_pcm_buf) {
            ESP_LOGE(LOG_TAG, "pcm_buf alloc failed");
            fclose(f); return;
        }
    }

    s_pcm_queue  = xQueueCreate(CONFIG_PCM_QUEUE_DEPTH, sizeof(pcm_chunk_t *));
    s_pcm_free_q = xQueueCreate(CONFIG_PCM_QUEUE_DEPTH, sizeof(pcm_chunk_t *));
    s_chunk_queue = xQueueCreate(CONFIG_SD_QUEUE_DEPTH, sizeof(chunk_t *));
    s_free_queue  = xQueueCreate(CONFIG_SD_QUEUE_DEPTH, sizeof(chunk_t *));
    if (!s_chunk_queue || !s_free_queue) {
        ESP_LOGE(LOG_TAG, "Queue create failed");
        delete_queue(&s_chunk_queue);
        delete_queue(&s_free_queue);
        fclose(f); return;
    }

    bool ok = true;
    for (int i = 0; i < CONFIG_SD_QUEUE_DEPTH; i++) {
        chunk_t *c = heap_caps_malloc(sizeof(chunk_t), MALLOC_CAP_SPIRAM);
        if (!c) {
            ESP_LOGE(LOG_TAG, "chunk[%d] alloc failed", i);
            ok = false; break;
        }
        xQueueSend(s_free_queue, &c, 0);
    }
    if (!ok) {
        drain_and_free_queue(s_free_queue);
        delete_queue(&s_chunk_queue);
        delete_queue(&s_free_queue);
        fclose(f); return;
    }
    for (int i = 0; i < CONFIG_PCM_QUEUE_DEPTH; i++) {
        pcm_chunk_t *pcm = heap_caps_malloc(sizeof(pcm_chunk_t), MALLOC_CAP_SPIRAM);
        if (!pcm) {
            ESP_LOGE(LOG_TAG, "pcm alloc failed");
            return;
        }
        xQueueSend(s_pcm_free_q, &pcm, 0);
    }

    reader_ctx_t ctx = { .file = f, .caller = xTaskGetCurrentTaskHandle() };
    xTaskCreatePinnedToCore(reader_task, "sd_reader", SD_READER_STACK_SIZE,
                            &ctx, SD_READER_PRIORITY, &s_reader_handle, 0);
    xTaskCreatePinnedToCore(i2s_task, "i2s_task", I2S_TASK_STACK_SIZE,
                        NULL, I2S_TASK_PRIORITY, &s_i2s_task_handle, 1);

    uint8_t     *read_ptr    = s_work_buf;
    int          bytes_left  = 0;
    bool         eof         = false;
    bool         first_frame = true;
    int          frame_count = 0;
    MP3FrameInfo info;
    int64_t      t_last      = esp_timer_get_time();

    while ((!eof || bytes_left > 0) && !s_stop_requested) {

        if (bytes_left < CONFIG_REFILL_THRESHOLD && !eof) {
            memmove(s_work_buf, read_ptr, bytes_left);
            read_ptr = s_work_buf;

            chunk_t *chunk = NULL;
            if (xQueueReceive(s_chunk_queue, &chunk, pdMS_TO_TICKS(500)) == pdTRUE) {
                if (chunk->len == 0) {
                    eof = true;
                } else {
                    memcpy(s_work_buf + bytes_left, chunk->data, chunk->len);
                    bytes_left += chunk->len;
                }
                xQueueSend(s_free_queue, &chunk, portMAX_DELAY);
            } else {
                ESP_LOGW(LOG_TAG, "SD timeout");
                eof = true;
            }
        }

        if (bytes_left == 0) break;

        const int offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (offset < 0) { bytes_left = 0; continue; }
        read_ptr   += offset;
        bytes_left -= offset;

        const int err = MP3Decode(s_decoder, &read_ptr, &bytes_left, s_pcm_buf, 0);

        const int64_t t_now = esp_timer_get_time();
        if (frame_count % 50 == 0) {
            ESP_LOGI(LOG_TAG,
                     "frame=%d gap=%" PRId64 " us sd_free=%d",
                     frame_count, t_now - t_last,
                     uxQueueSpacesAvailable(s_free_queue));
        }
        t_last = t_now;

        if (err == ERR_MP3_INDATA_UNDERFLOW) continue;
        if (err != ERR_MP3_NONE) {
            if (bytes_left > 0) { read_ptr++; bytes_left--; }
            continue;
        }

        MP3GetLastFrameInfo(s_decoder, &info);
        frame_count++;

        if (first_frame) {
            ESP_LOGI(LOG_TAG, "rate=%d ch=%d samps=%d",
                     info.samprate, info.nChans, info.outputSamps);
            i2s_output_set_sample_rate(info.samprate);
            first_frame = false;
        }

        int write_samples = info.outputSamps;
        if (info.nChans == 1) {
            for (int i = info.outputSamps - 1; i >= 0; i--) {
                s_pcm_buf[i * 2]     = s_pcm_buf[i];
                s_pcm_buf[i * 2 + 1] = s_pcm_buf[i];
            }
            write_samples = info.outputSamps * 2;
        }

        for (int i = 0; i < write_samples; i++) {
            float tmp = (float)s_pcm_buf[i] * s_volume;

            if (tmp > 32767.0f) tmp = 32767.0f;
            if (tmp < -32768.0f) tmp = -32768.0f;

            s_pcm_buf[i] = (int16_t)tmp;
        }

        pcm_chunk_t *pcm = NULL;
        xQueueReceive(s_pcm_free_q, &pcm, portMAX_DELAY);

        memcpy(pcm->data, s_pcm_buf, write_samples * sizeof(int16_t));
        pcm->samples = write_samples;

        xQueueSend(s_pcm_queue, &pcm, portMAX_DELAY);
    }

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_reader_handle = NULL;
    fclose(f);

    pcm_chunk_t *pcm = NULL;
    xQueueReceive(s_pcm_free_q, &pcm, portMAX_DELAY);

    pcm->samples = 0;
    xQueueSend(s_pcm_queue, &pcm, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(LOG_TAG, "Done, frames=%d", frame_count);

    drain_and_free_queue(s_free_queue);
    drain_and_free_queue(s_chunk_queue);

    delete_queue(&s_chunk_queue);
    delete_queue(&s_free_queue);

    drain_and_free_queue(s_pcm_free_q);
    drain_and_free_queue(s_pcm_queue);

    delete_queue(&s_pcm_queue);
    delete_queue(&s_pcm_free_q);
}
