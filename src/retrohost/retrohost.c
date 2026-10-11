/*
 * retrohost.c -- see include/vfx/retrohost.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "vfx/retrohost.h"
#include "libretro.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define dl_open(p) (void *)LoadLibraryA(p)
#define dl_sym(h, s) (void *)GetProcAddress((HMODULE)(h), s)
#define dl_close(h) FreeLibrary((HMODULE)(h))
#else
#include <dlfcn.h>
#define dl_open(p) dlopen(p, RTLD_NOW | RTLD_LOCAL)
#define dl_sym(h, s) dlsym(h, s)
#define dl_close(h) dlclose(h)
#endif

#define MAX_OPTIONS 128
#define MAX_DECLARED 256
#define MAX_VALUES 64

typedef struct Declared {
    char *key, *label;
    char *values[MAX_VALUES];
    int value_count;
} Declared;

typedef struct Core {
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*run)(void);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void (*vfx1_pose)(int, int, int, int, int, int);
    uint32_t *(*vfx1_stats)(void);
    void (*vfx1_tracker_gain)(double);
    void (*vfx1_serial_tracker)(int, int);
    void (*vfx1_steer)(int);
} Core;

struct RetroHost {
    void *lib;
    Core core;
    int initialised, loaded;
    char system_dir[1024], save_dir[1024];
    struct { char key[128]; char value[256]; } options[MAX_OPTIONS];
    int option_count, options_changed;
    Declared declared[MAX_DECLARED];
    int declared_count;

    struct retro_system_av_info av;
    enum retro_pixel_format format;

    uint8_t *frame;
    size_t frame_size;
    int frame_w, frame_h;
    uint64_t serial;

    uint8_t keys[RETROK_LAST];
    retro_keyboard_event_t keyboard_cb;
    int mouse_dx, mouse_dy, poll_dx, poll_dy;
    unsigned mouse_buttons;

    retrohost_audio_fn audio_fn; void *audio_user;
    retrohost_log_fn log_fn; void *log_user;
};

static RetroHost *g_host;       /* libretro's callbacks carry no user pointer */

static void host_log(int level, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_host && g_host->log_fn) g_host->log_fn(g_host->log_user, level, line);
    else fprintf(stderr, "%s", line);
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_host && g_host->log_fn) g_host->log_fn(g_host->log_user, (int)level, line);
    else fprintf(stderr, "core: %s", line);
}

static char *dup_range(const char *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}

static void declared_clear(RetroHost *h)
{
    int i, v;
    for (i = 0; i < h->declared_count; i++) {
        free(h->declared[i].key);
        free(h->declared[i].label);
        for (v = 0; v < h->declared[i].value_count; v++) free(h->declared[i].values[v]);
    }
    h->declared_count = 0;
}

/* "Label; value1|value2|..." */
static void declare_variables(RetroHost *h, const struct retro_variable *vars)
{
    declared_clear(h);
    for (; vars && vars->key && h->declared_count < MAX_DECLARED; vars++) {
        Declared *d = &h->declared[h->declared_count];
        const char *semi = vars->value ? strchr(vars->value, ';') : NULL;
        const char *p;
        memset(d, 0, sizeof *d);
        d->key = dup_range(vars->key, strlen(vars->key));
        if (!semi) { d->label = dup_range(vars->key, strlen(vars->key)); h->declared_count++; continue; }
        d->label = dup_range(vars->value, (size_t)(semi - vars->value));
        p = semi + 1;
        while (*p == ' ') p++;
        while (*p && d->value_count < MAX_VALUES) {
            const char *bar = strchr(p, '|');
            size_t n = bar ? (size_t)(bar - p) : strlen(p);
            d->values[d->value_count++] = dup_range(p, n);
            if (!bar) break;
            p = bar + 1;
        }
        h->declared_count++;
    }
}

static const char *option_get(RetroHost *h, const char *key)
{
    int i;
    for (i = 0; i < h->option_count; i++)
        if (!strcmp(h->options[i].key, key)) return h->options[i].value;
    return NULL;
}

static bool environment(unsigned cmd, void *data)
{
    RetroHost *h = g_host;
    if (!h) return false;
    switch (cmd & ~(unsigned)RETRO_ENVIRONMENT_EXPERIMENTAL) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        if (f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_RGB565) return false;
        h->format = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = h->system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        *(const char **)data = h->save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        v->value = option_get(h, v->key);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = h->options_changed != 0;
        h->options_changed = 0;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;              /* core falls back to SET_VARIABLES */
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        declare_variables(h, (const struct retro_variable *)data);
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        h->av = *(const struct retro_system_av_info *)data;
        return true;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        h->keyboard_cb = ((const struct retro_keyboard_callback *)data)->callback;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        host_log(RETRO_LOG_INFO, "core message: %s\n", ((const struct retro_message *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
        host_log(RETRO_LOG_INFO, "core message: %s\n", ((const struct retro_message_ext *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *(bool *)data = false;
        return true;
    default:
        return false;                        /* no HW render, VFS, MIDI, perf, disk control */
    }
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    RetroHost *h = g_host;
    size_t need = (size_t)width * height * 3u, x, y;
    if (!h || !data) return;                 /* NULL: frame duplicated */
    if (h->frame_size != need) {
        uint8_t *f = (uint8_t *)realloc(h->frame, need);
        if (!f) return;
        h->frame = f;
        h->frame_size = need;
    }
    h->frame_w = (int)width;
    h->frame_h = (int)height;
    for (y = 0; y < height; y++) {
        uint8_t *dst = h->frame + y * width * 3u;
        if (h->format == RETRO_PIXEL_FORMAT_XRGB8888) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)data + y * pitch);
            for (x = 0; x < width; x++) {
                uint32_t p = src[x];
                dst[0] = (uint8_t)(p >> 16); dst[1] = (uint8_t)(p >> 8); dst[2] = (uint8_t)p;
                dst += 3;
            }
        } else {
            const uint16_t *src = (const uint16_t *)((const uint8_t *)data + y * pitch);
            for (x = 0; x < width; x++) {
                uint16_t p = src[x];
                dst[0] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
                dst[1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
                dst[2] = (uint8_t)((p & 31) * 255 / 31);
                dst += 3;
            }
        }
    }
    h->serial++;
}

static void audio_sample(int16_t left, int16_t right)
{
    int16_t f[2];
    f[0] = left; f[1] = right;
    if (g_host && g_host->audio_fn) g_host->audio_fn(g_host->audio_user, f, 1);
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    if (g_host && g_host->audio_fn) g_host->audio_fn(g_host->audio_user, data, frames);
    return frames;
}

static void input_poll(void)
{
    RetroHost *h = g_host;
    if (!h) return;
    h->poll_dx = h->mouse_dx;
    h->poll_dy = h->mouse_dy;
    h->mouse_dx = h->mouse_dy = 0;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    RetroHost *h = g_host;
    (void)index;
    if (!h || port != 0) return 0;
    switch (device) {
    case RETRO_DEVICE_KEYBOARD:
        return (int16_t)(id < RETROK_LAST ? h->keys[id] : 0);
    case RETRO_DEVICE_MOUSE:
        switch (id) {
        case RETRO_DEVICE_ID_MOUSE_X: return (int16_t)h->poll_dx;
        case RETRO_DEVICE_ID_MOUSE_Y: return (int16_t)h->poll_dy;
        case RETRO_DEVICE_ID_MOUSE_LEFT: return (int16_t)!!(h->mouse_buttons & 1);
        case RETRO_DEVICE_ID_MOUSE_RIGHT: return (int16_t)!!(h->mouse_buttons & 2);
        case RETRO_DEVICE_ID_MOUSE_MIDDLE: return (int16_t)!!(h->mouse_buttons & 4);
        default: return 0;
        }
    default:
        return 0;
    }
}

#define SYM(field, name) do { \
    h->core.field = dl_sym(h->lib, name); \
    if (!h->core.field) { snprintf(err, err_size, "core has no %s", name); goto fail; } \
} while (0)

RetroHost *retrohost_open(const char *core_path, const char *system_dir, const char *save_dir,
                          char *err, size_t err_size)
{
    RetroHost *h;
    if (g_host) { snprintf(err, err_size, "a core is already open"); return NULL; }
    h = (RetroHost *)calloc(1, sizeof *h);
    if (!h) { snprintf(err, err_size, "out of memory"); return NULL; }
    h->lib = dl_open(core_path);
    if (!h->lib) {
#ifdef _WIN32
        snprintf(err, err_size, "cannot load %s", core_path);
#else
        snprintf(err, err_size, "cannot load %s: %s", core_path, dlerror());
#endif
        free(h);
        return NULL;
    }
    SYM(init, "retro_init");
    SYM(deinit, "retro_deinit");
    SYM(api_version, "retro_api_version");
    SYM(get_system_av_info, "retro_get_system_av_info");
    SYM(set_environment, "retro_set_environment");
    SYM(set_video_refresh, "retro_set_video_refresh");
    SYM(set_audio_sample, "retro_set_audio_sample");
    SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
    SYM(set_input_poll, "retro_set_input_poll");
    SYM(set_input_state, "retro_set_input_state");
    SYM(run, "retro_run");
    SYM(load_game, "retro_load_game");
    SYM(unload_game, "retro_unload_game");
    h->core.vfx1_pose = dl_sym(h->lib, "vfx1Pose");
    h->core.vfx1_stats = dl_sym(h->lib, "vfx1Stats");
    h->core.vfx1_tracker_gain = dl_sym(h->lib, "vfx1TrackerGain");
    h->core.vfx1_serial_tracker = dl_sym(h->lib, "vfx1SerialTracker");
    h->core.vfx1_steer = dl_sym(h->lib, "vfx1Steer");

    if (h->core.api_version() != RETRO_API_VERSION) {
        snprintf(err, err_size, "core API version %u, host %u", h->core.api_version(), RETRO_API_VERSION);
        goto fail;
    }
    snprintf(h->system_dir, sizeof h->system_dir, "%s", system_dir ? system_dir : ".");
    snprintf(h->save_dir, sizeof h->save_dir, "%s", save_dir ? save_dir : ".");
    h->format = RETRO_PIXEL_FORMAT_0RGB1555;

    g_host = h;
    h->core.set_environment(environment);
    h->core.set_video_refresh(video_refresh);
    h->core.set_audio_sample(audio_sample);
    h->core.set_audio_sample_batch(audio_batch);
    h->core.set_input_poll(input_poll);
    h->core.set_input_state(input_state);
    h->core.init();
    h->initialised = 1;
    return h;

fail:
    dl_close(h->lib);
    free(h);
    return NULL;
}

void retrohost_close(RetroHost *h)
{
    if (!h) return;
    if (h->loaded) h->core.unload_game();
    if (h->initialised) h->core.deinit();
    dl_close(h->lib);
    free(h->frame);
    declared_clear(h);
    if (g_host == h) g_host = NULL;
    free(h);
}

void retrohost_set_log(RetroHost *h, retrohost_log_fn fn, void *user) { h->log_fn = fn; h->log_user = user; }
void retrohost_set_audio(RetroHost *h, retrohost_audio_fn fn, void *user) { h->audio_fn = fn; h->audio_user = user; }

int retrohost_set_option(RetroHost *h, const char *key, const char *value)
{
    int i;
    for (i = 0; i < h->option_count; i++)
        if (!strcmp(h->options[i].key, key)) break;
    if (i == h->option_count) {
        if (h->option_count == MAX_OPTIONS) return 0;
        h->option_count++;
    }
    snprintf(h->options[i].key, sizeof h->options[i].key, "%s", key);
    snprintf(h->options[i].value, sizeof h->options[i].value, "%s", value);
    h->options_changed = 1;
    return 1;
}

int retrohost_load(RetroHost *h, const char *content_path)
{
    struct retro_game_info info;
    memset(&info, 0, sizeof info);
    info.path = content_path;
    if (!h->core.load_game(&info)) return 0;
    h->loaded = 1;
    h->core.get_system_av_info(&h->av);
    return 1;
}

void retrohost_unload(RetroHost *h)
{
    if (!h->loaded) return;
    h->core.unload_game();
    h->loaded = 0;
    memset(h->keys, 0, sizeof h->keys);
    h->mouse_dx = h->mouse_dy = 0;
    h->mouse_buttons = 0;
}

int retrohost_loaded(const RetroHost *h) { return h->loaded; }

int retrohost_option_count(const RetroHost *h) { return h->declared_count; }
const char *retrohost_option_key(const RetroHost *h, int i) { return i >= 0 && i < h->declared_count ? h->declared[i].key : NULL; }
const char *retrohost_option_label(const RetroHost *h, int i) { return i >= 0 && i < h->declared_count ? h->declared[i].label : NULL; }
int retrohost_option_value_count(const RetroHost *h, int i) { return i >= 0 && i < h->declared_count ? h->declared[i].value_count : 0; }

const char *retrohost_option_value(const RetroHost *h, int i, int v)
{
    if (i < 0 || i >= h->declared_count || v < 0 || v >= h->declared[i].value_count) return NULL;
    return h->declared[i].values[v];
}

const char *retrohost_option_current(const RetroHost *h, const char *key)
{
    int i;
    for (i = 0; i < h->option_count; i++)
        if (!strcmp(h->options[i].key, key)) return h->options[i].value;
    for (i = 0; i < h->declared_count; i++)
        if (!strcmp(h->declared[i].key, key)) return h->declared[i].value_count ? h->declared[i].values[0] : NULL;
    return NULL;
}

void retrohost_run(RetroHost *h) { h->core.run(); }
double retrohost_fps(const RetroHost *h) { return h->av.timing.fps; }
double retrohost_sample_rate(const RetroHost *h) { return h->av.timing.sample_rate; }

const uint8_t *retrohost_frame(const RetroHost *h, int *width, int *height, uint64_t *serial)
{
    if (width) *width = h->frame_w;
    if (height) *height = h->frame_h;
    if (serial) *serial = h->serial;
    return h->frame;
}

void retrohost_key(RetroHost *h, unsigned retrok, int down)
{
    if (retrok >= RETROK_LAST || h->keys[retrok] == (uint8_t)!!down) return;
    h->keys[retrok] = (uint8_t)!!down;
    if (h->keyboard_cb) h->keyboard_cb(down != 0, retrok, 0, 0);
}

void retrohost_mouse(RetroHost *h, int dx, int dy, unsigned buttons)
{
    h->mouse_dx += dx;
    h->mouse_dy += dy;
    h->mouse_buttons = buttons;
}

int retrohost_has_vfx1(const RetroHost *h) { return h->core.vfx1_pose != NULL; }

void retrohost_vfx1_pose(RetroHost *h, int yaw, int pitch, int roll, int puck_pitch, int puck_roll, int buttons)
{
    if (h->core.vfx1_pose) h->core.vfx1_pose(yaw, pitch, roll, puck_pitch, puck_roll, buttons);
}

void retrohost_vfx1_tracker_gain(RetroHost *h, double gain)
{
    if (h->core.vfx1_tracker_gain) h->core.vfx1_tracker_gain(gain);
}

void retrohost_vfx1_serial_tracker(RetroHost *h, int port, int kind)
{
    if (h->core.vfx1_serial_tracker) h->core.vfx1_serial_tracker(port, kind);
}

void retrohost_vfx1_steer(RetroHost *h, int on)
{
    if (h->core.vfx1_steer) h->core.vfx1_steer(on);
}

const uint32_t *retrohost_vfx1_stats(RetroHost *h)
{
    return h->core.vfx1_stats ? h->core.vfx1_stats() : NULL;
}
