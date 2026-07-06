#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

#define DEVICE_NAME "CHPN0001:00"
#define ANIMATION_FRAME_MS 16

typedef struct {
    Display *display;
    int screen;
    Window root;
    Window overlay;
    unsigned long overlay_pixel;
    int touch_fd;
    int lock_fd;
    char touch_path[PATH_MAX];
    char xinput_id[32];
    int pointer_x;
    int pointer_y;
    double current_x;
    double current_y;
    double anchor_x;
    double anchor_y;
    double down_since;
    double arm_deadline;
    double animation_started_at;
    bool have_x;
    bool have_y;
    bool finger_down;
    bool armed;
    bool cancelled;
    bool completed;
    bool triggered;
    bool input_frozen;
    bool overlay_visible;
    TouchRMBConfig config;
} DaemonState;

static volatile sig_atomic_t g_running = 1;
static const char *g_home = NULL;

static void handle_signal(int signo) {
    (void)signo;
    g_running = 0;
}

static double monotonic_seconds(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static const char *cache_dir(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    static char path[PATH_MAX];

    if (xdg && xdg[0] != '\0') {
        return xdg;
    }
    snprintf(path, sizeof(path), "%s/.cache", g_home);
    return path;
}

static bool build_cache_path(char *buffer, size_t size, const char *suffix) {
    int written = snprintf(buffer, size, "%s/%s", cache_dir(), suffix);
    return written >= 0 && (size_t)written < size;
}

static void sleep_milliseconds(long ms) {
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void log_message(const char *fmt, ...) {
    char path[PATH_MAX];
    FILE *handle;
    time_t now;
    struct tm tm_now;
    char stamp[32];
    va_list args;

    if (!build_cache_path(path, sizeof(path), "touchrmb.log")) {
        return;
    }
    handle = fopen(path, "a");
    if (!handle) {
        return;
    }

    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(handle, "%s ", stamp);
    va_start(args, fmt);
    vfprintf(handle, fmt, args);
    va_end(args);
    fputc('\n', handle);
    fclose(handle);
}

static bool distance_exceeded(double ax, double ay, double bx, double by, double threshold) {
    double dx = ax - bx;
    double dy = ay - by;

    return dx * dx + dy * dy > threshold * threshold;
}

static int run_command_capture(const char *command, char *buffer, size_t size) {
    FILE *pipe;
    size_t used = 0;

    if (size == 0) {
        return -1;
    }
    buffer[0] = '\0';
    pipe = popen(command, "r");
    if (!pipe) {
        return -1;
    }
    while (used + 1 < size && fgets(buffer + used, (int)(size - used), pipe)) {
        used = strlen(buffer);
    }
    pclose(pipe);
    return 0;
}

static void run_command_quiet(const char *command) {
    FILE *pipe = popen(command, "r");

    if (!pipe) {
        return;
    }
    while (fgetc(pipe) != EOF) {
    }
    pclose(pipe);
}

static void refresh_xinput_id(DaemonState *state) {
    char output[4096];
    char *line;
    char *saveptr = NULL;

    state->xinput_id[0] = '\0';
    if (run_command_capture("xinput list --short 2>/dev/null", output, sizeof(output)) != 0) {
        return;
    }

    line = strtok_r(output, "\n", &saveptr);
    while (line) {
        if (strstr(line, DEVICE_NAME) && strstr(line, "[slave  pointer")) {
            char *id = strstr(line, "id=");
            if (id) {
                snprintf(state->xinput_id, sizeof(state->xinput_id), "%d", atoi(id + 3));
                return;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
}

static bool get_pointer_location(int *x, int *y) {
    char output[256];
    char *line;
    char *saveptr = NULL;
    bool have_x = false;
    bool have_y = false;

    if (run_command_capture("xdotool getmouselocation --shell 2>/dev/null", output, sizeof(output)) != 0) {
        return false;
    }
    line = strtok_r(output, "\n", &saveptr);
    while (line) {
        if (strncmp(line, "X=", 2) == 0) {
            *x = atoi(line + 2);
            have_x = true;
        } else if (strncmp(line, "Y=", 2) == 0) {
            *y = atoi(line + 2);
            have_y = true;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return have_x && have_y;
}

static void freeze_pointer(DaemonState *state) {
    char command[128];

    if (state->input_frozen) {
        return;
    }
    if (!get_pointer_location(&state->pointer_x, &state->pointer_y)) {
        state->pointer_x = 0;
        state->pointer_y = 0;
    }
    if (state->xinput_id[0] == '\0') {
        refresh_xinput_id(state);
    }
    if (state->xinput_id[0] != '\0') {
        snprintf(command, sizeof(command), "xinput disable %s >/dev/null 2>&1", state->xinput_id);
        run_command_quiet(command);
    }
    state->input_frozen = true;
    log_message("freeze pointer=(%d,%d)", state->pointer_x, state->pointer_y);
}

static void unfreeze_pointer(DaemonState *state) {
    char command[128];

    if (!state->input_frozen) {
        return;
    }
    if (state->xinput_id[0] != '\0') {
        snprintf(command, sizeof(command), "xinput enable %s >/dev/null 2>&1", state->xinput_id);
        run_command_quiet(command);
    }
    state->input_frozen = false;
    log_message("unfreeze");
}

static void emit_right_click(void) {
    run_command_quiet("xdotool mousedown --clearmodifiers 3 >/dev/null 2>&1");
    sleep_milliseconds(30);
    run_command_quiet("xdotool mouseup --clearmodifiers 3 >/dev/null 2>&1");
}

static int open_touch_device(char *path, size_t path_size) {
    DIR *dir;
    struct dirent *entry;
    int fd = -1;

    dir = opendir("/dev/input");
    if (!dir) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char full_path[PATH_MAX];
        char name[256] = {0};

        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        snprintf(full_path, sizeof(full_path), "/dev/input/%s", entry->d_name);
        fd = open(full_path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && strcmp(name, DEVICE_NAME) == 0) {
            snprintf(path, path_size, "%s", full_path);
            closedir(dir);
            return fd;
        }
        close(fd);
        fd = -1;
    }

    closedir(dir);
    return -1;
}

static bool init_overlay(DaemonState *state) {
    XColor color;
    Colormap colormap;
    XSetWindowAttributes attrs;
    char hex_color[16];

    state->display = XOpenDisplay(NULL);
    if (!state->display) {
        log_message("XOpenDisplay failed");
        return false;
    }
    state->screen = DefaultScreen(state->display);
    state->root = RootWindow(state->display, state->screen);
    colormap = DefaultColormap(state->display, state->screen);

    touchrmb_config_format_color(&state->config, hex_color, sizeof(hex_color));
    if (!XParseColor(state->display, colormap, hex_color, &color) ||
        !XAllocColor(state->display, colormap, &color)) {
        state->overlay_pixel = WhitePixel(state->display, state->screen);
    } else {
        state->overlay_pixel = color.pixel;
    }

    attrs.override_redirect = True;
    attrs.background_pixel = state->overlay_pixel;
    attrs.border_pixel = state->overlay_pixel;
    state->overlay = XCreateWindow(
        state->display,
        state->root,
        0,
        0,
        1,
        1,
        0,
        CopyFromParent,
        InputOutput,
        CopyFromParent,
        CWOverrideRedirect | CWBackPixel | CWBorderPixel,
        &attrs
    );
    XFlush(state->display);
    return true;
}

static void hide_overlay(DaemonState *state) {
    if (!state->overlay_visible) {
        return;
    }
    XUnmapWindow(state->display, state->overlay);
    XFlush(state->display);
    state->overlay_visible = false;
}

static void show_overlay_progress(DaemonState *state, double progress) {
    int size;
    int line_width;
    int left;
    int top;
    XRectangle rects[4];

    if (!state->display) {
        return;
    }
    if (progress < 0.0) {
        progress = 0.0;
    }
    if (progress > 1.0) {
        progress = 1.0;
    }

    line_width = state->config.line_width;
    size = (int)(10.0 + progress * (double)(state->config.square_size - 10) + 0.5);
    if (size < line_width * 2) {
        size = line_width * 2;
    }

    left = state->pointer_x - size / 2;
    top = state->pointer_y - size / 2;

    rects[0] = (XRectangle){0, 0, (unsigned short)size, (unsigned short)line_width};
    rects[1] = (XRectangle){0, (short)(size - line_width), (unsigned short)size, (unsigned short)line_width};
    rects[2] = (XRectangle){0, 0, (unsigned short)line_width, (unsigned short)size};
    rects[3] = (XRectangle){(short)(size - line_width), 0, (unsigned short)line_width, (unsigned short)size};

    XMoveResizeWindow(state->display, state->overlay, left, top, (unsigned int)size, (unsigned int)size);
    XShapeCombineRectangles(state->display, state->overlay, ShapeBounding, 0, 0, rects, 4, ShapeSet, 0);
#ifdef ShapeInput
    XShapeCombineRectangles(state->display, state->overlay, ShapeInput, 0, 0, NULL, 0, ShapeSet, 0);
#endif
    XClearWindow(state->display, state->overlay);
    if (!state->overlay_visible) {
        XMapRaised(state->display, state->overlay);
        state->overlay_visible = true;
    } else {
        XRaiseWindow(state->display, state->overlay);
    }
    XFlush(state->display);
}

static void reset_press(DaemonState *state) {
    state->finger_down = false;
    state->armed = false;
    state->cancelled = false;
    state->completed = false;
    state->triggered = false;
    state->down_since = 0.0;
    state->arm_deadline = 0.0;
    state->animation_started_at = 0.0;
    state->have_x = false;
    state->have_y = false;
    hide_overlay(state);
    unfreeze_pointer(state);
}

static void on_press(DaemonState *state) {
    double now = monotonic_seconds();

    state->finger_down = true;
    state->armed = false;
    state->cancelled = false;
    state->completed = false;
    state->triggered = false;
    state->down_since = now;
    state->arm_deadline = now + (double)state->config.pre_arm_ms / 1000.0;
    state->animation_started_at = 0.0;
    state->anchor_x = state->current_x;
    state->anchor_y = state->current_y;
    hide_overlay(state);
    log_message("press anchor=(%.1f,%.1f)", state->anchor_x, state->anchor_y);
}

static void on_release(DaemonState *state) {
    double held_for = state->down_since > 0.0 ? monotonic_seconds() - state->down_since : 0.0;

    if (state->completed && !state->triggered) {
        hide_overlay(state);
        XSync(state->display, False);
        sleep_milliseconds(30);
        emit_right_click();
        state->triggered = true;
        log_message(
            "right click emitted held_for=%.3f pointer=(%d,%d)",
            held_for,
            state->pointer_x,
            state->pointer_y
        );
    } else {
        log_message("release before completion held_for=%.3f", held_for);
    }
    reset_press(state);
}

static void handle_motion(DaemonState *state) {
    if (!state->finger_down || state->armed || state->triggered || state->cancelled || !state->have_x || !state->have_y) {
        return;
    }
    if (!distance_exceeded(
            state->anchor_x,
            state->anchor_y,
            state->current_x,
            state->current_y,
            18.0)) {
        return;
    }

    state->cancelled = true;
    hide_overlay(state);
    log_message(
        "long press cancelled by motion start=(%.1f,%.1f) current=(%.1f,%.1f)",
        state->anchor_x,
        state->anchor_y,
        state->current_x,
        state->current_y
    );
}

static void process_event(DaemonState *state, const struct input_event *event) {
    if (event->type == EV_ABS) {
        if (event->code == ABS_X || event->code == ABS_MT_POSITION_X) {
            state->current_x = (double)event->value;
            state->have_x = true;
            handle_motion(state);
        } else if (event->code == ABS_Y || event->code == ABS_MT_POSITION_Y) {
            state->current_y = (double)event->value;
            state->have_y = true;
            handle_motion(state);
        }
    } else if (event->type == EV_KEY && event->code == BTN_TOUCH) {
        if (event->value == 1) {
            on_press(state);
        } else if (event->value == 0) {
            on_release(state);
        }
    }
}

static void attach_touch_device(DaemonState *state) {
    if (state->touch_fd >= 0) {
        return;
    }
    state->touch_fd = open_touch_device(state->touch_path, sizeof(state->touch_path));
    if (state->touch_fd >= 0) {
        refresh_xinput_id(state);
        log_message("attached %s xinput=%s", state->touch_path, state->xinput_id[0] ? state->xinput_id : "none");
    }
}

static void detach_touch_device(DaemonState *state, const char *reason) {
    if (reason) {
        log_message("%s", reason);
    }
    if (state->touch_fd >= 0) {
        close(state->touch_fd);
        state->touch_fd = -1;
    }
    reset_press(state);
}

static void update_timers(DaemonState *state, double now) {
    double progress;

    if (state->finger_down && !state->armed && !state->cancelled && !state->triggered && state->arm_deadline > 0.0 && now >= state->arm_deadline) {
        state->armed = true;
        state->animation_started_at = now;
        freeze_pointer(state);
        show_overlay_progress(state, 0.0);
        log_message("armed");
    }

    if (!state->finger_down || !state->armed || state->triggered) {
        return;
    }

    progress = (now - state->animation_started_at) / ((double)state->config.animation_ms / 1000.0);
    if (progress > 1.0) {
        progress = 1.0;
    }
    show_overlay_progress(state, progress);
    if (!state->completed && progress >= 1.0) {
        state->completed = true;
        log_message("completed");
    }
}

static int compute_poll_timeout_ms(const DaemonState *state, double now) {
    if (state->touch_fd < 0) {
        return 1000;
    }
    if (state->finger_down && !state->armed && !state->cancelled && !state->triggered) {
        double remaining = state->arm_deadline - now;
        if (remaining <= 0.0) {
            return 0;
        }
        return (int)(remaining * 1000.0);
    }
    if (state->finger_down && state->armed && !state->completed && !state->triggered) {
        return ANIMATION_FRAME_MS;
    }
    return -1;
}

static bool acquire_lock(DaemonState *state) {
    char path[PATH_MAX];
    char buffer[32];
    ssize_t ignored;

    if (!build_cache_path(path, sizeof(path), "touchrmb.lock")) {
        return false;
    }
    state->lock_fd = open(path, O_CREAT | O_RDWR, 0600);
    if (state->lock_fd < 0) {
        return false;
    }
    if (flock(state->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        return false;
    }
    snprintf(buffer, sizeof(buffer), "%ld\n", (long)getpid());
    if (ftruncate(state->lock_fd, 0) != 0) {
        return false;
    }
    ignored = write(state->lock_fd, buffer, strlen(buffer));
    if (ignored < 0) {
        return false;
    }
    return true;
}

static void cleanup(DaemonState *state) {
    hide_overlay(state);
    unfreeze_pointer(state);
    if (state->touch_fd >= 0) {
        close(state->touch_fd);
        state->touch_fd = -1;
    }
    if (state->overlay) {
        XDestroyWindow(state->display, state->overlay);
    }
    if (state->display) {
        XCloseDisplay(state->display);
    }
    if (state->lock_fd >= 0) {
        close(state->lock_fd);
    }
}

int main(void) {
    DaemonState state;

    memset(&state, 0, sizeof(state));
    state.touch_fd = -1;
    state.lock_fd = -1;
    g_home = getenv("HOME");
    if (!g_home || g_home[0] == '\0') {
        fprintf(stderr, "HOME is not set\n");
        return 1;
    }

    touchrmb_config_defaults(&state.config);
    touchrmb_config_load(&state.config);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (!acquire_lock(&state)) {
        fprintf(stderr, "another instance is already running\n");
        return 1;
    }
    log_message("instance lock acquired pid=%ld", (long)getpid());

    if (!init_overlay(&state)) {
        cleanup(&state);
        return 1;
    }

    while (g_running) {
        struct pollfd pfd;
        int poll_count = 0;
        int timeout_ms;
        double now = monotonic_seconds();

        attach_touch_device(&state);
        update_timers(&state, now);
        timeout_ms = compute_poll_timeout_ms(&state, now);

        memset(&pfd, 0, sizeof(pfd));
        if (state.touch_fd >= 0) {
            pfd.fd = state.touch_fd;
            pfd.events = POLLIN | POLLERR | POLLHUP;
            poll_count = 1;
        }

        if (poll(&pfd, poll_count, timeout_ms) < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_message("poll failed: %s", strerror(errno));
            break;
        }

        if (state.touch_fd >= 0 && (pfd.revents & (POLLERR | POLLHUP))) {
            detach_touch_device(&state, "touch device disconnected");
            continue;
        }

        if (state.touch_fd >= 0 && (pfd.revents & POLLIN)) {
            struct input_event events[32];
            ssize_t bytes;

            while ((bytes = read(state.touch_fd, events, sizeof(events))) > 0) {
                size_t count = (size_t)bytes / sizeof(events[0]);
                size_t index;

                for (index = 0; index < count; ++index) {
                    process_event(&state, &events[index]);
                }
            }
            if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                detach_touch_device(&state, strerror(errno));
            }
        }
    }

    cleanup(&state);
    return 0;
}
