#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_PRE_ARM_MS 150
#define DEFAULT_ANIMATION_MS 250
#define DEFAULT_SQUARE_SIZE 75
#define DEFAULT_LINE_WIDTH 1
#define DEFAULT_COLOR_R 56
#define DEFAULT_COLOR_G 153
#define DEFAULT_COLOR_B 250

#define MIN_PRE_ARM_MS 50
#define MAX_PRE_ARM_MS 2000
#define MIN_ANIMATION_MS 50
#define MAX_ANIMATION_MS 2000
#define MIN_SQUARE_SIZE 12
#define MAX_SQUARE_SIZE 160
#define MIN_LINE_WIDTH 1
#define MAX_LINE_WIDTH 12

static int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static char *trim(char *value) {
    char *end;

    while (*value != '\0' && isspace((unsigned char)*value)) {
        ++value;
    }
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return value;
}

static bool parse_int(const char *value, int *out) {
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *trim(end) != '\0') {
        return false;
    }
    if (parsed < -2147483647L || parsed > 2147483647L) {
        return false;
    }
    *out = (int)parsed;
    return true;
}

static bool config_dir_path(char *buffer, size_t size) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int written;

    if (xdg && xdg[0] != '\0') {
        written = snprintf(buffer, size, "%s/touchrmb", xdg);
    } else if (home && home[0] != '\0') {
        written = snprintf(buffer, size, "%s/.config/touchrmb", home);
    } else {
        return false;
    }
    return written >= 0 && (size_t)written < size;
}

bool touchrmb_config_path(char *buffer, size_t size) {
    char dir[PATH_MAX];
    int written;

    if (!config_dir_path(dir, sizeof(dir))) {
        return false;
    }
    written = snprintf(buffer, size, "%s/config.ini", dir);
    return written >= 0 && (size_t)written < size;
}

void touchrmb_config_defaults(TouchRMBConfig *config) {
    config->pre_arm_ms = DEFAULT_PRE_ARM_MS;
    config->animation_ms = DEFAULT_ANIMATION_MS;
    config->square_size = DEFAULT_SQUARE_SIZE;
    config->line_width = DEFAULT_LINE_WIDTH;
    config->color_r = DEFAULT_COLOR_R;
    config->color_g = DEFAULT_COLOR_G;
    config->color_b = DEFAULT_COLOR_B;
}

void touchrmb_config_clamp(TouchRMBConfig *config) {
    config->pre_arm_ms = clamp_int(config->pre_arm_ms, MIN_PRE_ARM_MS, MAX_PRE_ARM_MS);
    config->animation_ms = clamp_int(config->animation_ms, MIN_ANIMATION_MS, MAX_ANIMATION_MS);
    config->square_size = clamp_int(config->square_size, MIN_SQUARE_SIZE, MAX_SQUARE_SIZE);
    config->line_width = clamp_int(config->line_width, MIN_LINE_WIDTH, MAX_LINE_WIDTH);
}

void touchrmb_config_format_color(const TouchRMBConfig *config, char *buffer, size_t size) {
    snprintf(
        buffer,
        size,
        "#%02X%02X%02X",
        (unsigned int)config->color_r,
        (unsigned int)config->color_g,
        (unsigned int)config->color_b
    );
}

bool touchrmb_config_parse_color(TouchRMBConfig *config, const char *value) {
    unsigned int r;
    unsigned int g;
    unsigned int b;

    if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) != 3) {
        return false;
    }
    config->color_r = (uint8_t)r;
    config->color_g = (uint8_t)g;
    config->color_b = (uint8_t)b;
    return true;
}

bool touchrmb_config_load(TouchRMBConfig *config) {
    char path[PATH_MAX];
    FILE *handle;
    char line[256];

    touchrmb_config_defaults(config);
    if (!touchrmb_config_path(path, sizeof(path))) {
        return false;
    }
    handle = fopen(path, "r");
    if (!handle) {
        return false;
    }

    while (fgets(line, sizeof(line), handle)) {
        char *eq;
        char *key;
        char *value;
        int parsed;

        key = trim(line);
        if (key[0] == '\0' || key[0] == '#') {
            continue;
        }
        eq = strchr(key, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        value = trim(eq + 1);
        key = trim(key);

        if (strcmp(key, "pre_arm_ms") == 0) {
            if (parse_int(value, &parsed)) {
                config->pre_arm_ms = parsed;
            }
        } else if (strcmp(key, "animation_ms") == 0) {
            if (parse_int(value, &parsed)) {
                config->animation_ms = parsed;
            }
        } else if (strcmp(key, "square_size") == 0) {
            if (parse_int(value, &parsed)) {
                config->square_size = parsed;
            }
        } else if (strcmp(key, "line_width") == 0) {
            if (parse_int(value, &parsed)) {
                config->line_width = parsed;
            }
        } else if (strcmp(key, "color") == 0) {
            touchrmb_config_parse_color(config, value);
        }
    }

    fclose(handle);
    touchrmb_config_clamp(config);
    return true;
}

static bool ensure_dir(const char *path) {
    if (mkdir(path, 0700) == 0) {
        return true;
    }
    return errno == EEXIST;
}

bool touchrmb_config_save(const TouchRMBConfig *config) {
    char dir[PATH_MAX];
    char path[PATH_MAX];
    char color[16];
    FILE *handle;
    TouchRMBConfig copy = *config;

    touchrmb_config_clamp(&copy);
    if (!config_dir_path(dir, sizeof(dir)) || !touchrmb_config_path(path, sizeof(path))) {
        return false;
    }
    if (!ensure_dir(dir)) {
        return false;
    }

    handle = fopen(path, "w");
    if (!handle) {
        return false;
    }
    touchrmb_config_format_color(&copy, color, sizeof(color));
    fprintf(handle, "pre_arm_ms=%d\n", copy.pre_arm_ms);
    fprintf(handle, "animation_ms=%d\n", copy.animation_ms);
    fprintf(handle, "square_size=%d\n", copy.square_size);
    fprintf(handle, "line_width=%d\n", copy.line_width);
    fprintf(handle, "color=%s\n", color);
    fclose(handle);
    return true;
}
