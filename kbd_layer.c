/*
 * kbd_layer.c
 *
 * CapsLock-activated cursor-movement layer for Ubuntu/GNOME (X11 or Wayland).
 * Operates at the evdev/uinput level below the display server, so it works
 * the same regardless of compositor (Mutter/Wayland included).
 *
 * The key->direction/speed mapping now lives in an external config file
 * that is re-read on SIGHUP, so you can tune it without recompiling or
 * even restarting the daemon.
 *
 * Build:
 *   sudo apt install libevdev-dev
 *   gcc -O2 -Wall -std=gnu11 -o kbd_layer kbd_layer.c -lpthread $(pkg-config --cflags --libs libevdev)
 *
 * Run:
 *   sudo ./kbd_layer /dev/input/by-path/YOUR-KEYBOARD-event-kbd kbd_layer.conf
 *
 * Reload the mapping live after editing the config file:
 *   sudo kill -HUP $(pgrep kbd_layer)
 */

#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* ---------------------------------------------------------------------
 * Mapping data structures
 * ------------------------------------------------------------------- */

typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } direction_t;

typedef struct {
    int         keycode;
    direction_t dir;
    int         speed;     /* pixels moved per tick while held */
} key_map_t;

#define MAX_KEYS 32
#define TICK_US  16000      /* ~60 Hz movement update */

static key_map_t g_map[MAX_KEYS];
static size_t    g_num_keys = 0;
static pthread_mutex_t g_map_mutex = PTHREAD_MUTEX_INITIALIZER;

static atomic_bool g_held[MAX_KEYS];
/* True for a key currently mid-press that we forwarded through as a real
 * keystroke (because the layer wasn't active yet when it was pressed).
 * Its eventual release MUST also be forwarded, regardless of layer state
 * by then, or the compositor thinks the key is stuck down forever. */
static atomic_bool g_passthrough_active[MAX_KEYS];
static atomic_bool g_layer_active = false;
static atomic_bool g_running = true;
static atomic_bool g_reload_requested = false;

/* How long (ms) a mapped key's press is held in limbo waiting to see if
 * CapsLock follows. Tunable via a "GRACE <ms>" line in the config file,
 * reloadable live like everything else. Larger = more reliably resolves
 * simultaneous presses correctly; smaller = less latency on normal typing
 * of these same keys. */
static atomic_int g_grace_ms = 50;

typedef struct {
    bool pending;
    struct timespec press_time;
} pending_state_t;
static pending_state_t g_pending[MAX_KEYS];

static long elapsed_ms(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

static struct libevdev *g_dev = NULL;
static struct libevdev_uinput *g_uidev = NULL;
static char g_config_path[512] = "kbd_layer.conf";

/* ---------------------------------------------------------------------
 * Key name -> Linux keycode table (covers the whole alphanumeric board
 * plus common punctuation, so you can experiment with any key, not just
 * the original 12).
 * ------------------------------------------------------------------- */
static const struct { char ch; int code; } g_char_table[] = {
    {'q',KEY_Q},{'w',KEY_W},{'e',KEY_E},{'r',KEY_R},{'t',KEY_T},{'y',KEY_Y},
    {'u',KEY_U},{'i',KEY_I},{'o',KEY_O},{'p',KEY_P},
    {'a',KEY_A},{'s',KEY_S},{'d',KEY_D},{'f',KEY_F},{'g',KEY_G},{'h',KEY_H},
    {'j',KEY_J},{'k',KEY_K},{'l',KEY_L},
    {'z',KEY_Z},{'x',KEY_X},{'c',KEY_C},{'v',KEY_V},{'b',KEY_B},{'n',KEY_N},{'m',KEY_M},
    {'0',KEY_0},{'1',KEY_1},{'2',KEY_2},{'3',KEY_3},{'4',KEY_4},
    {'5',KEY_5},{'6',KEY_6},{'7',KEY_7},{'8',KEY_8},{'9',KEY_9},
    {';',KEY_SEMICOLON},{'\'',KEY_APOSTROPHE},{'/',KEY_SLASH},
    {',',KEY_COMMA},{'.',KEY_DOT},
    {'[',KEY_LEFTBRACE},{']',KEY_RIGHTBRACE},{'-',KEY_MINUS},{'=',KEY_EQUAL},
    {'`',KEY_GRAVE},{'\\',KEY_BACKSLASH},
};
#define CHAR_TABLE_LEN (sizeof(g_char_table) / sizeof(g_char_table[0]))

static int name_to_keycode(const char *name) {
    if (strlen(name) == 1) {
        char c = (char)tolower((unsigned char)name[0]);
        for (size_t i = 0; i < CHAR_TABLE_LEN; i++)
            if (g_char_table[i].ch == c)
                return g_char_table[i].code;
    } else {
        if (strcasecmp(name, "SPACE") == 0) return KEY_SPACE;
        if (strcasecmp(name, "ENTER") == 0) return KEY_ENTER;
        if (strcasecmp(name, "TAB") == 0)   return KEY_TAB;
    }
    return -1;
}

static int name_to_direction(const char *name, direction_t *out) {
    if (strcasecmp(name, "UP") == 0    || strcasecmp(name, "U") == 0) { *out = DIR_UP;    return 0; }
    if (strcasecmp(name, "DOWN") == 0  || strcasecmp(name, "D") == 0) { *out = DIR_DOWN;  return 0; }
    if (strcasecmp(name, "LEFT") == 0  || strcasecmp(name, "L") == 0) { *out = DIR_LEFT;  return 0; }
    if (strcasecmp(name, "RIGHT") == 0 || strcasecmp(name, "R") == 0) { *out = DIR_RIGHT; return 0; }
    return -1;
}

static const char *dir_name(direction_t d) {
    switch (d) {
        case DIR_UP:    return "UP";
        case DIR_DOWN:  return "DOWN";
        case DIR_LEFT:  return "LEFT";
        case DIR_RIGHT: return "RIGHT";
    }
    return "?";
}

/* ---------------------------------------------------------------------
 * Config file loading
 *
 * Format, one binding per line:
 *   <key> <direction> <speed>
 *
 * <key>       a single character (h, j, ;, ', /, ...) or SPACE/ENTER/TAB
 * <direction> UP / DOWN / LEFT / RIGHT  (or U/D/L/R)
 * <speed>     integer pixels moved per ~16ms tick while held
 *
 * Optional directive, one per file, sets the CapsLock race window:
 *   GRACE <milliseconds>
 *
 * Lines starting with # and blank lines are ignored.
 * ------------------------------------------------------------------- */
static void print_mapping(const key_map_t *map, size_t n) {
    printf("---- active mapping (%zu keys) ----\n", n);
    for (size_t i = 0; i < n; i++)
        printf("  code %3d  ->  %-5s  speed %d\n", map[i].keycode, dir_name(map[i].dir), map[i].speed);
    printf("------------------------------------\n");
}

static int load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "config: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    key_map_t tmp[MAX_KEYS];
    size_t count = 0;
    int grace_val = atomic_load(&g_grace_ms);
    char line[256];
    int lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char tok1[32];
        if (sscanf(p, "%31s", tok1) == 1 && strcasecmp(tok1, "GRACE") == 0) {
            int ms;
            if (sscanf(p, "%31s %d", tok1, &ms) == 2 && ms >= 0) {
                grace_val = ms;
            } else {
                fprintf(stderr, "config: %s:%d: bad GRACE directive, ignoring\n", path, lineno);
            }
            continue;
        }

        char keytok[32], dirtok[32];
        int speed;
        int n = sscanf(p, "%31s %31s %d", keytok, dirtok, &speed);
        if (n != 3) {
            fprintf(stderr, "config: %s:%d: expected '<key> <direction> <speed>', skipping\n", path, lineno);
            continue;
        }

        int code = name_to_keycode(keytok);
        if (code < 0) {
            fprintf(stderr, "config: %s:%d: unknown key '%s', skipping\n", path, lineno, keytok);
            continue;
        }
        direction_t dir;
        if (name_to_direction(dirtok, &dir) < 0) {
            fprintf(stderr, "config: %s:%d: unknown direction '%s', skipping\n", path, lineno, dirtok);
            continue;
        }
        if (count >= MAX_KEYS) {
            fprintf(stderr, "config: %s:%d: too many bindings (max %d), ignoring rest\n", path, lineno, MAX_KEYS);
            break;
        }
        tmp[count].keycode = code;
        tmp[count].dir = dir;
        tmp[count].speed = speed;
        count++;
    }
    fclose(f);

    if (count == 0) {
        fprintf(stderr, "config: %s produced zero valid bindings, keeping previous mapping\n", path);
        return -1;
    }

    pthread_mutex_lock(&g_map_mutex);
    memcpy(g_map, tmp, sizeof(key_map_t) * count);
    g_num_keys = count;
    for (size_t i = 0; i < MAX_KEYS; i++) {
        atomic_store(&g_held[i], false);
        atomic_store(&g_passthrough_active[i], false);
        g_pending[i].pending = false;
    }
    pthread_mutex_unlock(&g_map_mutex);
    atomic_store(&g_grace_ms, grace_val);

    printf("config: loaded %zu bindings from %s (grace=%dms)\n", count, path, grace_val);
    print_mapping(tmp, count);
    return 0;
}

static void clear_all_held(void) {
    for (size_t i = 0; i < MAX_KEYS; i++)
        atomic_store(&g_held[i], false);
}

static int find_key_index_locked(int code) {
    for (size_t i = 0; i < g_num_keys; i++)
        if (g_map[i].keycode == code)
            return (int)i;
    return -1;
}

/* ---------------------------------------------------------------------
 * Movement thread
 * ------------------------------------------------------------------- */
static void *mover_thread(void *arg) {
    (void)arg;
    while (atomic_load(&g_running)) {
        usleep(TICK_US);

        if (!atomic_load(&g_layer_active))
            continue;

        int dx = 0, dy = 0;
        pthread_mutex_lock(&g_map_mutex);
        for (size_t i = 0; i < g_num_keys; i++) {
            if (!atomic_load(&g_held[i]))
                continue;
            int s = g_map[i].speed;
            switch (g_map[i].dir) {
                case DIR_UP:    dy -= s; break;
                case DIR_DOWN:  dy += s; break;
                case DIR_LEFT:  dx -= s; break;
                case DIR_RIGHT: dx += s; break;
            }
        }
        pthread_mutex_unlock(&g_map_mutex);

        if (dx == 0 && dy == 0)
            continue;

        libevdev_uinput_write_event(g_uidev, EV_REL, REL_X, dx);
        libevdev_uinput_write_event(g_uidev, EV_REL, REL_Y, dy);
        libevdev_uinput_write_event(g_uidev, EV_SYN, SYN_REPORT, 0);
    }
    return NULL;
}

static void handle_sigint(int sig) {
    (void)sig;
    atomic_store(&g_running, false);
}

static void handle_sighup(int sig) {
    (void)sig;
    atomic_store(&g_reload_requested, true);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s /dev/input/by-path/YOUR-KEYBOARD-event-kbd [config_file]\n", argv[0]);
        return 1;
    }
    if (argc == 3)
        strncpy(g_config_path, argv[2], sizeof(g_config_path) - 1);

    if (load_config(g_config_path) < 0) {
        fprintf(stderr, "failed to load initial config, exiting\n");
        return 1;
    }

    int fd = open(argv[1], O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open input device");
        return 1;
    }

    if (libevdev_new_from_fd(fd, &g_dev) < 0) {
        fprintf(stderr, "failed to init libevdev on %s\n", argv[1]);
        return 1;
    }
    printf("Reading from: %s\n", libevdev_get_name(g_dev));

    if (libevdev_grab(g_dev, LIBEVDEV_GRAB) < 0) {
        fprintf(stderr, "failed to grab device (are you root?)\n");
        return 1;
    }

    struct libevdev *uidev_proto = libevdev_new();
    libevdev_set_name(uidev_proto, "kbd-layer-virtual-input");

    for (int code = 0; code <= KEY_MAX; code++) {
        if (libevdev_has_event_code(g_dev, EV_KEY, code))
            libevdev_enable_event_code(uidev_proto, EV_KEY, code, NULL);
    }
    libevdev_enable_event_type(uidev_proto, EV_REL);
    libevdev_enable_event_code(uidev_proto, EV_REL, REL_X, NULL);
    libevdev_enable_event_code(uidev_proto, EV_REL, REL_Y, NULL);
    libevdev_enable_event_code(uidev_proto, EV_KEY, BTN_LEFT, NULL);
    libevdev_enable_event_code(uidev_proto, EV_KEY, BTN_RIGHT, NULL);

    int uifd = open("/dev/uinput", O_RDWR);
    if (uifd < 0) {
        perror("open /dev/uinput");
        return 1;
    }
    if (libevdev_uinput_create_from_device(uidev_proto, uifd, &g_uidev) < 0) {
        fprintf(stderr, "failed to create uinput device\n");
        return 1;
    }
    libevdev_free(uidev_proto);

    for (size_t i = 0; i < MAX_KEYS; i++) {
        atomic_init(&g_held[i], false);
        atomic_init(&g_passthrough_active[i], false);
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGHUP, handle_sighup);

    pthread_t mover;
    pthread_create(&mover, NULL, mover_thread, NULL);

    printf("Layer active. Hold CapsLock + your mapped keys to move the pointer.\n");
    printf("Edit %s and run 'kill -HUP %d' to reload the mapping live. Ctrl+C to quit.\n",
           g_config_path, getpid());

    while (atomic_load(&g_running)) {
        if (atomic_exchange(&g_reload_requested, false)) {
            load_config(g_config_path);
        }

        /* Resolve any buffered key whose grace window has lapsed without
         * CapsLock showing up: it was just an ordinary keystroke. */
        {
            int grace = atomic_load(&g_grace_ms);
            bool any_pending = false;
            pthread_mutex_lock(&g_map_mutex);
            for (size_t i = 0; i < g_num_keys; i++) {
                if (!g_pending[i].pending) continue;
                if (elapsed_ms(&g_pending[i].press_time) >= grace) {
                    g_pending[i].pending = false;
                    libevdev_uinput_write_event(g_uidev, EV_KEY, g_map[i].keycode, 1);
                    libevdev_uinput_write_event(g_uidev, EV_SYN, SYN_REPORT, 0);
                    atomic_store(&g_passthrough_active[i], true);
                } else {
                    any_pending = true;
                }
            }
            pthread_mutex_unlock(&g_map_mutex);

            /* Block until the kernel actually has an event for us -- no
             * fixed polling delay in the common case. While a key is
             * buffered we poll tightly so its grace window is honored
             * precisely; otherwise 200ms is just a safety net for
             * shutdown/reload flags (signals normally interrupt poll()
             * immediately anyway). */
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int pr = poll(&pfd, 1, any_pending ? 5 : 200);
            if (pr <= 0)
                continue;
        }

        struct input_event ev;
        int rc = libevdev_next_event(g_dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);

        if (rc == -EAGAIN)
            continue;
        if (rc < 0)
            break;

        if (ev.type == EV_KEY && ev.code == KEY_CAPSLOCK) {
            if (ev.value == 1) {
                atomic_store(&g_layer_active, true);
                /* Promote any key currently buffered (pressed just before
                 * CapsLock registered) into movement instead of letting
                 * it fall through as a real keystroke. */
                int grace = atomic_load(&g_grace_ms);
                pthread_mutex_lock(&g_map_mutex);
                for (size_t i = 0; i < g_num_keys; i++) {
                    if (!g_pending[i].pending) continue;
                    g_pending[i].pending = false;
                    if (elapsed_ms(&g_pending[i].press_time) <= grace) {
                        atomic_store(&g_held[i], true);
                    } else {
                        /* Grace already lapsed right before CapsLock showed
                         * up; treat as a normal keystroke instead. */
                        libevdev_uinput_write_event(g_uidev, EV_KEY, g_map[i].keycode, 1);
                        libevdev_uinput_write_event(g_uidev, EV_SYN, SYN_REPORT, 0);
                        atomic_store(&g_passthrough_active[i], true);
                    }
                }
                pthread_mutex_unlock(&g_map_mutex);
            } else if (ev.value == 0) {
                atomic_store(&g_layer_active, false);
                clear_all_held();
            }
            continue;
        }

        if (ev.type == EV_KEY) {
            pthread_mutex_lock(&g_map_mutex);
            int idx = find_key_index_locked(ev.code);
            pthread_mutex_unlock(&g_map_mutex);

            if (idx >= 0) {
                bool passthrough = atomic_load(&g_passthrough_active[idx]);

                if (ev.value == 1) {                 /* press */
                    if (passthrough) {
                        /* Shouldn't normally happen, but stay consistent. */
                        libevdev_uinput_write_event(g_uidev, ev.type, ev.code, ev.value);
                    } else if (atomic_load(&g_layer_active)) {
                        atomic_store(&g_held[idx], true);
                    } else {
                        /* Ambiguous: buffer the press. Resolved either by
                         * CapsLock arriving (see above) or by the grace
                         * sweep below if it doesn't. */
                        g_pending[idx].pending = true;
                        clock_gettime(CLOCK_MONOTONIC, &g_pending[idx].press_time);
                    }
                } else if (ev.value == 0) {          /* release */
                    if (passthrough) {
                        atomic_store(&g_passthrough_active[idx], false);
                        libevdev_uinput_write_event(g_uidev, ev.type, ev.code, ev.value);
                    } else if (g_pending[idx].pending) {
                        /* Released before resolving -- it was just a quick
                         * ordinary tap. Send the whole press+release now. */
                        g_pending[idx].pending = false;
                        libevdev_uinput_write_event(g_uidev, EV_KEY, ev.code, 1);
                        libevdev_uinput_write_event(g_uidev, EV_SYN, SYN_REPORT, 0);
                        libevdev_uinput_write_event(g_uidev, EV_KEY, ev.code, 0);
                        libevdev_uinput_write_event(g_uidev, EV_SYN, SYN_REPORT, 0);
                    } else {
                        atomic_store(&g_held[idx], false);
                    }
                } else {                             /* autorepeat (value 2) */
                    if (passthrough)
                        libevdev_uinput_write_event(g_uidev, ev.type, ev.code, ev.value);
                    /* else: suppressed -- pending or held, mover thread /
                     * grace sweep handles it */
                }
                continue;
            }
        }

        libevdev_uinput_write_event(g_uidev, ev.type, ev.code, ev.value);
    }

    pthread_join(mover, NULL);
    libevdev_grab(g_dev, LIBEVDEV_UNGRAB);
    libevdev_uinput_destroy(g_uidev);
    libevdev_free(g_dev);
    close(fd);
    return 0;
}
