#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
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
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

#define ANIMATION_FRAME_MS 16
#define INITIAL_OVERLAY_SIZE 10
#define MOTION_THRESHOLD 18
#define MOTION_THRESHOLD_SQ (MOTION_THRESHOLD * MOTION_THRESHOLD)
#define PREFERRED_TOUCH_NAME "CHPN0001:00"

typedef enum {
    ROTATION_NORMAL = 0,
    ROTATION_LEFT,
    ROTATION_RIGHT,
    ROTATION_INVERTED,
} DisplayRotation;

typedef struct {
    Display *display;
    int screen;
    Window root;
    Window overlay;
    unsigned long overlay_pixel;
    int touch_fd;
    int lock_fd;
    int touch_xi_id;
    int screen_width;
    int screen_height;
    int abs_x_min;
    int abs_x_max;
    int abs_y_min;
    int abs_y_max;
    int current_x;
    int current_y;
    int anchor_x;
    int anchor_y;
    int pointer_x;
    int pointer_y;
    unsigned long long arm_deadline_ms;
    unsigned long long animation_started_ms;
    bool have_x;
    bool have_y;
    bool have_abs_x;
    bool have_abs_y;
    bool have_xi2;
    bool finger_down;
    bool armed;
    bool cancelled;
    bool completed;
    bool triggered;
    bool touch_grabbed;
    bool overlay_visible;
    char touch_name[256];
    DisplayRotation rotation;
    TouchRMBConfig config;
} DaemonState;

static volatile sig_atomic_t g_running = 1;
static const char *g_home = NULL;

static void handle_signal(int signo) {
    (void)signo;
    g_running = 0;
}

static unsigned long long monotonic_milliseconds(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
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

static bool test_bit(const unsigned long *bits, size_t bit) {
    size_t index = bit / (sizeof(bits[0]) * 8U);
    size_t offset = bit % (sizeof(bits[0]) * 8U);

    return (bits[index] & (1UL << offset)) != 0;
}

static bool is_touchscreen_device(int fd) {
    unsigned long ev_bits[(EV_MAX + (sizeof(unsigned long) * 8U)) / (sizeof(unsigned long) * 8U)] = {0};
    unsigned long key_bits[(KEY_MAX + (sizeof(unsigned long) * 8U)) / (sizeof(unsigned long) * 8U)] = {0};
    unsigned long abs_bits[(ABS_MAX + (sizeof(unsigned long) * 8U)) / (sizeof(unsigned long) * 8U)] = {0};
    unsigned long prop_bits[(INPUT_PROP_MAX + (sizeof(unsigned long) * 8U)) / (sizeof(unsigned long) * 8U)] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 || !test_bit(ev_bits, EV_ABS)) {
        return false;
    }
    (void)ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
    (void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
    (void)ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), prop_bits);

    return test_bit(prop_bits, INPUT_PROP_DIRECT) &&
        test_bit(key_bits, BTN_TOUCH) &&
        ((test_bit(abs_bits, ABS_MT_POSITION_X) && test_bit(abs_bits, ABS_MT_POSITION_Y)) ||
            (test_bit(abs_bits, ABS_X) && test_bit(abs_bits, ABS_Y)));
}

static void load_abs_axis(int fd, int mt_code, int fallback_code, int *minimum, int *maximum, bool *available) {
    struct input_absinfo info;

    *available = false;
    if (ioctl(fd, EVIOCGABS(mt_code), &info) == 0 || ioctl(fd, EVIOCGABS(fallback_code), &info) == 0) {
        *minimum = info.minimum;
        *maximum = info.maximum;
        *available = true;
    }
}

static void load_abs_info(DaemonState *state) {
    load_abs_axis(state->touch_fd, ABS_MT_POSITION_X, ABS_X, &state->abs_x_min, &state->abs_x_max, &state->have_abs_x);
    load_abs_axis(state->touch_fd, ABS_MT_POSITION_Y, ABS_Y, &state->abs_y_min, &state->abs_y_max, &state->have_abs_y);
}

static int find_touch_xi_device(DaemonState *state) {
    XIDeviceInfo *devices;
    int count = 0;
    int index;
    int device_id = -1;

    if (!state->have_xi2 || state->touch_name[0] == '\0') {
        return -1;
    }

    devices = XIQueryDevice(state->display, XIAllDevices, &count);
    if (!devices) {
        return -1;
    }

    for (index = 0; index < count; ++index) {
        if (devices[index].use == XISlavePointer &&
            devices[index].name &&
            strcmp(devices[index].name, state->touch_name) == 0) {
            device_id = devices[index].deviceid;
            break;
        }
    }

    XIFreeDeviceInfo(devices);
    return device_id;
}

static DisplayRotation detect_display_rotation(DaemonState *state) {
    Rotation current = RR_Rotate_0;

    (void)XRRRotations(state->display, state->screen, &current);
    if (current & RR_Rotate_90) {
        return ROTATION_RIGHT;
    }
    if (current & RR_Rotate_270) {
        return ROTATION_LEFT;
    }
    if (current & RR_Rotate_180) {
        return ROTATION_INVERTED;
    }
    return ROTATION_NORMAL;
}

static bool get_pointer_location(DaemonState *state, int *x, int *y) {
    Window root_return;
    Window child_return;
    int root_x;
    int root_y;
    int win_x;
    int win_y;
    unsigned int mask_return;

    if (!XQueryPointer(
            state->display,
            state->root,
            &root_return,
            &child_return,
            &root_x,
            &root_y,
            &win_x,
            &win_y,
            &mask_return)) {
        return false;
    }

    *x = root_x;
    *y = root_y;
    return true;
}

static bool map_touch_to_screen(DaemonState *state, int *x, int *y) {
    double nx;
    double ny;
    double sx;
    double sy;

    if (!state->have_abs_x || !state->have_abs_y ||
        state->abs_x_max <= state->abs_x_min ||
        state->abs_y_max <= state->abs_y_min) {
        return false;
    }

    nx = (double)(state->current_x - state->abs_x_min) / (double)(state->abs_x_max - state->abs_x_min);
    ny = (double)(state->current_y - state->abs_y_min) / (double)(state->abs_y_max - state->abs_y_min);

    if (nx < 0.0) {
        nx = 0.0;
    } else if (nx > 1.0) {
        nx = 1.0;
    }
    if (ny < 0.0) {
        ny = 0.0;
    } else if (ny > 1.0) {
        ny = 1.0;
    }

    switch (state->rotation) {
    case ROTATION_RIGHT:
        sx = 1.0 - ny;
        sy = nx;
        break;
    case ROTATION_LEFT:
        sx = ny;
        sy = 1.0 - nx;
        break;
    case ROTATION_INVERTED:
        sx = 1.0 - nx;
        sy = 1.0 - ny;
        break;
    case ROTATION_NORMAL:
    default:
        sx = nx;
        sy = ny;
        break;
    }

    *x = (int)(sx * (double)(state->screen_width - 1) + 0.5);
    *y = (int)(sy * (double)(state->screen_height - 1) + 0.5);
    return true;
}

static void grab_touch_device(DaemonState *state) {
    unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = {0};
    XIEventMask mask;

    if (state->touch_grabbed || !state->have_xi2 || state->touch_xi_id < 0) {
        return;
    }

    mask.deviceid = state->touch_xi_id;
    mask.mask_len = (int)sizeof(mask_bits);
    mask.mask = mask_bits;

    if (XIGrabDevice(
            state->display,
            state->touch_xi_id,
            state->root,
            CurrentTime,
            None,
            XIGrabModeAsync,
            XIGrabModeAsync,
            False,
            &mask) == Success) {
        state->touch_grabbed = true;
        XSync(state->display, False);
    }
}

static void ungrab_touch_device(DaemonState *state) {
    if (!state->touch_grabbed) {
        return;
    }

    (void)XIUngrabDevice(state->display, state->touch_xi_id, CurrentTime);
    XSync(state->display, False);
    state->touch_grabbed = false;
}

static void emit_right_click_at(DaemonState *state, int x, int y) {
    XWarpPointer(state->display, None, state->root, 0, 0, 0, 0, x, y);
    XSync(state->display, False);
    XTestFakeButtonEvent(state->display, Button3, True, CurrentTime);
    XTestFakeButtonEvent(state->display, Button3, False, CurrentTime);
    XSync(state->display, False);
}

static void hide_overlay(DaemonState *state);

static void release_left_button(DaemonState *state) {
    XTestFakeButtonEvent(state->display, Button1, False, CurrentTime);
    XSync(state->display, False);
}

static void cancel_pre_arm_press(DaemonState *state) {
    state->cancelled = true;
    hide_overlay(state);
}

static void cancel_armed_press(DaemonState *state) {
    state->cancelled = true;
    state->armed = false;
    state->completed = false;
    hide_overlay(state);
    release_left_button(state);
}

static void hide_overlay(DaemonState *state) {
    if (!state->overlay_visible) {
        return;
    }

    XUnmapWindow(state->display, state->overlay);
    XFlush(state->display);
    state->overlay_visible = false;
}

static void show_overlay_size(DaemonState *state, int size) {
    int line_width = state->config.line_width;
    int left;
    int top;
    XRectangle rects[4];

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
    XClearWindow(state->display, state->overlay);

    if (!state->overlay_visible) {
        XMapRaised(state->display, state->overlay);
        state->overlay_visible = true;
    }

    XFlush(state->display);
}

static void reset_press(DaemonState *state) {
    state->have_x = false;
    state->have_y = false;
    state->finger_down = false;
    state->armed = false;
    state->cancelled = false;
    state->completed = false;
    state->triggered = false;
    state->arm_deadline_ms = 0;
    state->animation_started_ms = 0;
    hide_overlay(state);
    ungrab_touch_device(state);
}

static void start_press(DaemonState *state, unsigned long long now_ms) {
    state->finger_down = true;
    state->armed = false;
    state->cancelled = false;
    state->completed = false;
    state->triggered = false;
    state->arm_deadline_ms = now_ms + (unsigned long long)state->config.pre_arm_ms;
    state->animation_started_ms = 0;
    state->anchor_x = state->current_x;
    state->anchor_y = state->current_y;
    hide_overlay(state);
}

static void finish_press(DaemonState *state) {
    if (state->completed && !state->triggered) {
        hide_overlay(state);
        emit_right_click_at(state, state->pointer_x, state->pointer_y);
        state->triggered = true;
    }
    reset_press(state);
}

static void handle_motion(DaemonState *state) {
    long dx;
    long dy;

    if (!state->finger_down || state->cancelled || state->triggered || !state->have_x || !state->have_y) {
        return;
    }

    dx = (long)state->current_x - (long)state->anchor_x;
    dy = (long)state->current_y - (long)state->anchor_y;
    if (dx * dx + dy * dy > MOTION_THRESHOLD_SQ) {
        if (state->armed) {
            cancel_armed_press(state);
        } else {
            cancel_pre_arm_press(state);
        }
    }
}

static void process_event(DaemonState *state, const struct input_event *event) {
    if (event->type == EV_ABS) {
        if (event->code == ABS_X || event->code == ABS_MT_POSITION_X) {
            state->current_x = event->value;
            state->have_x = true;
            handle_motion(state);
        } else if (event->code == ABS_Y || event->code == ABS_MT_POSITION_Y) {
            state->current_y = event->value;
            state->have_y = true;
            handle_motion(state);
        }
        return;
    }

    if (event->type == EV_KEY && event->code == BTN_TOUCH) {
        if (event->value == 1) {
            start_press(state, monotonic_milliseconds());
        } else if (event->value == 0) {
            finish_press(state);
        }
    }
}

static void select_touch_device(DaemonState *state, int fd, const char *name) {
    state->touch_fd = fd;
    state->touch_xi_id = -1;
    snprintf(state->touch_name, sizeof(state->touch_name), "%s", name);
    load_abs_info(state);
}

static int open_touch_device(DaemonState *state) {
    DIR *dir;
    struct dirent *entry;
    int fallback_fd = -1;
    char fallback_name[256] = {0};

    dir = opendir("/dev/input");
    if (!dir) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        char name[256] = {0};
        int fd;

        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 || !is_touchscreen_device(fd)) {
            close(fd);
            continue;
        }

        if (strcmp(name, PREFERRED_TOUCH_NAME) == 0) {
            if (fallback_fd >= 0) {
                close(fallback_fd);
            }
            closedir(dir);
            select_touch_device(state, fd, name);
            return fd;
        }

        if (fallback_fd < 0) {
            fallback_fd = fd;
            snprintf(fallback_name, sizeof(fallback_name), "%s", name);
        } else {
            close(fd);
        }
    }

    closedir(dir);
    if (fallback_fd >= 0) {
        select_touch_device(state, fallback_fd, fallback_name);
    }
    return fallback_fd;
}

static bool init_overlay(DaemonState *state) {
    XColor color;
    Colormap colormap;
    XSetWindowAttributes attrs;
    char hex_color[16];
    int xi_major = 2;
    int xi_minor = 0;

    state->display = XOpenDisplay(NULL);
    if (!state->display) {
        return false;
    }

    state->screen = DefaultScreen(state->display);
    state->root = RootWindow(state->display, state->screen);
    state->screen_width = DisplayWidth(state->display, state->screen);
    state->screen_height = DisplayHeight(state->display, state->screen);
    state->rotation = detect_display_rotation(state);
    state->have_xi2 = XIQueryVersion(state->display, &xi_major, &xi_minor) == Success;

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
#ifdef ShapeInput
    XShapeCombineRectangles(state->display, state->overlay, ShapeInput, 0, 0, NULL, 0, ShapeSet, 0);
#endif
    XFlush(state->display);
    return true;
}

static void attach_touch_device(DaemonState *state) {
    if (state->touch_fd >= 0) {
        return;
    }

    if (open_touch_device(state) >= 0) {
        state->touch_xi_id = find_touch_xi_device(state);
    }
}

static void detach_touch_device(DaemonState *state) {
    if (state->touch_fd >= 0) {
        close(state->touch_fd);
        state->touch_fd = -1;
    }
    state->touch_xi_id = -1;
    state->touch_name[0] = '\0';
    state->have_abs_x = false;
    state->have_abs_y = false;
    reset_press(state);
}

static void arm_press(DaemonState *state, unsigned long long now_ms) {
    if (!get_pointer_location(state, &state->pointer_x, &state->pointer_y) &&
        !map_touch_to_screen(state, &state->pointer_x, &state->pointer_y)) {
        state->pointer_x = 0;
        state->pointer_y = 0;
    }

    state->armed = true;
    state->animation_started_ms = now_ms;
    grab_touch_device(state);
    /* The initial touch press may already be interpreted as a primary-button hold.
       Release it once the long-press gesture takes ownership to avoid desktop drag selection. */
    release_left_button(state);
    show_overlay_size(state, INITIAL_OVERLAY_SIZE);
}

static void update_timers(DaemonState *state, unsigned long long now_ms) {
    unsigned long long elapsed;
    unsigned long long duration;
    int extra_size;
    int size;

    if (state->finger_down && !state->armed && !state->cancelled && !state->triggered &&
        state->arm_deadline_ms > 0 && now_ms >= state->arm_deadline_ms) {
        arm_press(state, now_ms);
    }

    if (!state->finger_down || !state->armed || state->triggered) {
        return;
    }

    duration = (unsigned long long)(state->config.animation_ms > 0 ? state->config.animation_ms : 1);
    elapsed = now_ms > state->animation_started_ms ? now_ms - state->animation_started_ms : 0;
    if (elapsed > duration) {
        elapsed = duration;
    }

    extra_size = state->config.square_size - INITIAL_OVERLAY_SIZE;
    size = INITIAL_OVERLAY_SIZE;
    if (extra_size > 0) {
        size += (int)((elapsed * (unsigned long long)extra_size + duration / 2ULL) / duration);
    }
    show_overlay_size(state, size);

    if (!state->completed && elapsed >= duration) {
        state->completed = true;
    }
}

static int compute_poll_timeout_ms(const DaemonState *state, unsigned long long now_ms) {
    if (state->touch_fd < 0) {
        return 1000;
    }

    if (state->finger_down && !state->armed && !state->cancelled && !state->triggered) {
        if (now_ms >= state->arm_deadline_ms) {
            return 0;
        }
        return (int)(state->arm_deadline_ms - now_ms);
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
    if (state->lock_fd < 0 || flock(state->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        return false;
    }

    snprintf(buffer, sizeof(buffer), "%ld\n", (long)getpid());
    if (ftruncate(state->lock_fd, 0) != 0) {
        return false;
    }

    ignored = write(state->lock_fd, buffer, strlen(buffer));
    return ignored >= 0;
}

static void cleanup(DaemonState *state) {
    reset_press(state);
    if (state->touch_fd >= 0) {
        close(state->touch_fd);
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
    state.touch_xi_id = -1;
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
    if (!init_overlay(&state)) {
        cleanup(&state);
        return 1;
    }

    while (g_running) {
        struct pollfd pfd;
        int poll_count = 0;
        int timeout_ms;
        unsigned long long now_ms = monotonic_milliseconds();

        attach_touch_device(&state);
        update_timers(&state, now_ms);
        timeout_ms = compute_poll_timeout_ms(&state, now_ms);

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
            break;
        }

        if (state.touch_fd >= 0 && (pfd.revents & (POLLERR | POLLHUP))) {
            detach_touch_device(&state);
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
                detach_touch_device(&state);
            }
        }
    }

    cleanup(&state);
    return 0;
}
