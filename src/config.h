#ifndef TOUCHRMB_CONFIG_H
#define TOUCHRMB_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int pre_arm_ms;
    int animation_ms;
    int square_size;
    int line_width;
    uint8_t color_r;
    uint8_t color_g;
    uint8_t color_b;
} TouchRMBConfig;

void touchrmb_config_defaults(TouchRMBConfig *config);
void touchrmb_config_clamp(TouchRMBConfig *config);
bool touchrmb_config_load(TouchRMBConfig *config);
bool touchrmb_config_save(const TouchRMBConfig *config);
bool touchrmb_config_path(char *buffer, size_t size);
void touchrmb_config_format_color(const TouchRMBConfig *config, char *buffer, size_t size);
bool touchrmb_config_parse_color(TouchRMBConfig *config, const char *value);

#endif
