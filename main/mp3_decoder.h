#pragma once

void mp3_decode_file(const char *path);

void mp3_decoder_stop(void);

void mp3_stop_i2s(void);

void system_mute(void);

void system_unmute(void);

void user_mute_switch(void);

void update_volume(float volume);
