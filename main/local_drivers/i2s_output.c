#include "driver/i2s_std.h"
#include "i2s_output.h"

#include "portmacro.h"

static i2s_chan_handle_t tx_chan;

void i2s_output_init(const uint32_t sample_rate) {
    const i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_AUTO, I2S_ROLE_MASTER
    );
    i2s_new_channel(&chan_cfg, &tx_chan, NULL);

    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO
        ),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_BCLK_GPIO_PIN,
            .ws = CONFIG_LRC_GPIO_PIN,
            .dout = CONFIG_DIN_GPIO_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };

    i2s_channel_init_std_mode(tx_chan, &std_config);
    i2s_channel_enable(tx_chan);
}

void i2s_output_write(const int16_t *pcm, const size_t samples) {
    size_t written = 0;
    i2s_channel_write(tx_chan, pcm, samples * sizeof(int16_t), &written, portMAX_DELAY);
}

void i2s_output_set_sample_rate(const uint32_t rate) {
    i2s_channel_disable(tx_chan);
    const i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_reconfig_std_clock(tx_chan, &clk);
    i2s_channel_enable(tx_chan);
}
