#include "ir_rx.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define IR_DECODE_MARGIN_US  400

static const char *TAG = "IR_RX";

static rmt_symbol_word_t raw_symbols[CONFIG_RAW_BUF_SIZE];

// Callbacks queue
static QueueHandle_t s_ir_queue;

// Default callback
void default_ir_callback(const uint32_t code) {
    ESP_LOGI(TAG, "Received: 0x%08" PRIX32, code);
}

static ir_callback_t s_ir_callback = default_ir_callback;

void set_ir_callback(const ir_callback_t callback) {
    s_ir_callback = callback;
}

bool IRAM_ATTR rmt_rx_done_callback(rmt_channel_handle_t channel,
                                            const rmt_rx_done_event_data_t *edata,
                                            void *user_ctx) {
    BaseType_t high_task_woken = pdFALSE;
    const QueueHandle_t queue = user_ctx;

    xQueueSendFromISR(queue, edata, &high_task_woken);

    return high_task_woken == pdTRUE;
}

// NEC decoding
bool check_duration(const uint32_t signal_us, const uint32_t target_us) {
    return signal_us >= target_us - CONFIG_IR_DECODE_MARGIN_US &&
           signal_us <= target_us + CONFIG_IR_DECODE_MARGIN_US;
}

static bool nec_decode(const rmt_symbol_word_t *syms,
                        size_t sym_cnt,
                        uint32_t *out_code)
{
    if (sym_cnt < 2) return false;

    /* Lead pulse */
    if (!check_duration(syms[0].duration0, CONFIG_NEC_LEAD_HI) ||
        !check_duration(syms[0].duration1, CONFIG_NEC_LEAD_LO))
        return false;

    uint32_t code = 0;
    int bit = 0;

    for (size_t i = 1; i < sym_cnt; i++) {
        uint32_t hi = syms[i].duration0;
        uint32_t lo = syms[i].duration1;

        /* Паразитный глитч — пропускаем */
        if (hi < 100) continue;

        /* Все 32 бита собраны */
        if (bit == 32) break;

        /* Проверяем маркер бита */
        if (!check_duration(hi, CONFIG_NEC_BIT_HI)) return false;

        /* Финальный стоп-символ: lo=0 означает конец сигнала */
        if (lo == 0) {
            bit++;
            break;
        }

        if (check_duration(lo, CONFIG_NEC_ONE_LO)) {
            code |= (1u << bit);
        } else if (check_duration(lo, CONFIG_NEC_ZERO_LO)) {
            /* бит = 0, ничего не делаем */
        } else {
            return false;
        }
        bit++;
    }

    if (bit != 32) return false;

    *out_code = code;
    return true;
}

bool nec_is_repeat(const rmt_symbol_word_t *syms, const size_t sym_cnt) {
    if (sym_cnt != 2) return false;
    return check_duration(syms[0].duration0, CONFIG_NEC_LEAD_HI) &&
           check_duration(syms[0].duration1, CONFIG_NEC_RPT_LO);
}

void ir_rx_task(void *arg) {
    const rmt_channel_handle_t rx_chan = arg;

    const rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns =   1250,    // <1.25 мкс — noise, ignoring
        .signal_range_max_ns = 12000000,  // >12 мс — frame end
    };

    ESP_LOGI(TAG, "IR receiver running on GPIO %d", CONFIG_IR_RX_GPIO_PIN);

    /* Запускаем первый приём */
    ESP_ERROR_CHECK(rmt_receive(rx_chan, raw_symbols, sizeof(raw_symbols), &rx_cfg));

    rmt_rx_done_event_data_t evt;

    while (1) {
        if (xQueueReceive(s_ir_queue, &evt, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (!(evt.num_symbols == 0 || evt.received_symbols[0].duration0 < 8000)) {
                if (nec_is_repeat(evt.received_symbols, evt.num_symbols)) {
                    ESP_LOGI(TAG, "Repeat (last code held)");
                } else {
                    uint32_t code = 0;
                    if (nec_decode(evt.received_symbols, evt.num_symbols, &code)) {
                        s_ir_callback(code);
                    } else {
                        ESP_LOGW(TAG, "Decode failed, raw symbols (%zu):",
                                 evt.num_symbols);
                        for (size_t i = 0; i < evt.num_symbols; i++) {
                            ESP_LOGI(TAG, "  [%2zu] +%4" PRIu32 "us  -%4" PRIu32 "us",
                                     i,
                                     evt.received_symbols[i].duration0,
                                     evt.received_symbols[i].duration1);
                        }
                    }
                }
            }

            ESP_ERROR_CHECK(rmt_receive(rx_chan, raw_symbols,
                                            sizeof(raw_symbols), &rx_cfg));
        }
    }
}

void ir_rx_task_init(void) {
    /* Creating queue */
    s_ir_queue = xQueueCreate(CONFIG_IR_RX_QUEUE_DEPTH,
                               sizeof(rmt_rx_done_event_data_t));
    assert(s_ir_queue != NULL);

    /* Setting up RMT RX-channel */
    const rmt_rx_channel_config_t chan_cfg = {
        .gpio_num          = CONFIG_IR_RX_GPIO_PIN,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = CONFIG_RMT_CLK_RES_HZ,
        .mem_block_symbols = CONFIG_IR_RX_MEM_BLOCK_SYM,
    };
    rmt_channel_handle_t rx_chan = NULL;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&chan_cfg, &rx_chan));

    /* Register callback */
    const rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rmt_rx_done_callback,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_chan, &cbs,
                                                     s_ir_queue));

    /* Enabling RMT */
    ESP_ERROR_CHECK(rmt_enable(rx_chan));

    /* Creating task */
    xTaskCreatePinnedToCore(ir_rx_task, "ir_rx_task", 4096, (void*)rx_chan, 5, NULL, 0);
}