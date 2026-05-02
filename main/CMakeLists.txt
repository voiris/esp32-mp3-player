idf_component_register(
        SRCS
            "main.c"
            "local_drivers/i2s_output.c"
            "mp3_decoder.c"
            "local_drivers/sd_card_mount.c"
            "local_drivers/ir_rx.c"
            "local_drivers/ssd1306.c"
            "player.c"
            "utils.c"
            "local_drivers/id3.c"
        INCLUDE_DIRS "."
        REQUIRES driver fatfs esp-libhelix-mp3 esp_timer
)
