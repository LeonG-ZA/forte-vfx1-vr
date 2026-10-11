/*
 * launcher.c -- see launcher.h.
 */
#define _GNU_SOURCE
#include "vfx/launcher.h"
#include "fetch.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

/* item ids */
enum {
    ID_RESUME = 1, ID_GAMES, ID_DOWNLOADS, ID_CORE, ID_VFX1, ID_UNLOAD, ID_QUIT,
    ID_BACK, ID_REFRESH,
    ID_FOV, ID_DISTANCE, ID_STEREO, ID_MOUSE, ID_MOUSE_SPEED, ID_PUCK_HAND, ID_TRACKER_GAIN, ID_LCD, ID_TRACKER_PORT, ID_STEER,
    ID_STEREO_AUTO, ID_STEREO_FORCE, ID_STEREO_THRESHOLD, ID_RESET_GAME, ID_MODE_KEY, ID_FAKE3D,
    ID_GAME_CORE_BASE = 900,        /* + index into GAME_CORE_KEYS */
    ID_GAME_BASE = 1000,            /* + index into games */
    ID_SERVER_BASE = 2000,          /* + index into server_games */
    ID_OPTION_BASE = 3000           /* + core option index */
};

static const double FOVS[] = { 35.5, 45, 60, 80, 97 };
static const double DISTANCES[] = { 3.35, 2.5, 2.0, 1.5 };
static const double MOUSE_SPEEDS[] = { 0.25, 0.5, 1, 2, 4 };
static const double TRACKER_GAINS[] = { 1, 2, 4, 8, 16, 32 };
static const char *const TRACKER_NAMES[TRACKER_MODES] = {
    "as the zip sets", "i-glasses COM1", "i-glasses COM2", "CyberMaxx COM1", "CyberMaxx COM2" };
static const char *const STEREO_NAMES[STEREO_MODES] = { "off", "VFX1 pages", "interlaced lines" };
static const char *const THRESHOLD_NAMES[5] = {
    "1 (stays stereo)", "2", "3 (default)", "4", "5 (goes flat sooner)" };
static const char *const MODE_KEY_NAMES[MODEKEY_COUNT] = {
    "none", "F9 or left controller Y", "Scroll Lock or left stick click", "Pause"
};
static const char *const FAKE3D_NAMES[FAKE3D_LEVELS] = { "off", "subtle", "medium", "strong" };
static const char *const MOUSE_NAMES[MOUSE_MODES] = { "off", "CyberPuck tilt", "point right", "point left" };

/* DOSBox Pure options a game can override, the name it is saved under, and
 * whether a change needs the game restarted */
static const char *const GAME_CORE_KEYS[LAUNCHER_GAME_CORE] = {
    "dosbox_pure_cycles", "dosbox_pure_cpu_type", "dosbox_pure_cpu_core" };
static const char *const GAME_CORE_NAMES[LAUNCHER_GAME_CORE] = { "cycles", "cpu_type", "cpu_core" };
static const char *const GAME_CORE_LABELS[LAUNCHER_GAME_CORE] = {
    "Emulated performance (cycles)", "CPU type (reloads)", "CPU core" };
static const int GAME_CORE_RESTART[LAUNCHER_GAME_CORE] = { 0, 1, 0 };

#define FOOTER " Stick: move   A: select   Stick left/right: change   B: back   Menu: close"

/* ------------------------------------------------------------ settings file */

static void config_path(const Launcher *l, char *out, size_t size)
{
    snprintf(out, size, "%s/vfx1quest.cfg", l->files_dir);
}

/* Present while a game resumed at start-up is starting: still there at the
 * next start-up means it crashed the app, so it is not resumed again. */
static void starting_path(const Launcher *l, char *out, size_t size)
{
    snprintf(out, size, "%s/vfx1quest.starting", l->files_dir);
}

/* ------------------------------------------------------------ per-game stereo */

static int find_game_stereo(const Launcher *l, const char *name)
{
    int i;
    for (i = 0; i < l->stereo_count; i++)
        if (!strcmp(l->stereo_game[i], name)) return i;
    return -1;
}

static void set_game_stereo(Launcher *l, const char *name, int mode)
{
    int i = find_game_stereo(l, name);
    if (mode < 0 || mode >= STEREO_MODES || !name[0]) return;
    if (i < 0) {
        if (l->stereo_count >= LAUNCHER_MAX_GAMES) return;
        i = l->stereo_count++;
        snprintf(l->stereo_game[i], LAUNCHER_NAME, "%s", name);
        l->tracker_mode[i] = TRACKER_FROM_ZIP;
        l->steer_mode[i] = 2;
    }
    l->stereo_mode[i] = mode;
}

static int stereo_hint(const char *name);

static int game_entry(Launcher *l, const char *name)
{
    int i = find_game_stereo(l, name);
    if (i < 0 && name[0] && l->stereo_count < LAUNCHER_MAX_GAMES) {
        i = l->stereo_count++;
        snprintf(l->stereo_game[i], LAUNCHER_NAME, "%s", name);
        l->stereo_mode[i] = stereo_hint(name);
        l->tracker_mode[i] = TRACKER_FROM_ZIP;
        l->steer_mode[i] = 2;
    }
    return i;
}

static void set_game_tracker(Launcher *l, const char *name, int mode)
{
    int i;
    if (mode < 0 || mode >= TRACKER_MODES) return;
    i = game_entry(l, name);
    if (i >= 0) l->tracker_mode[i] = mode;
}

static void set_game_steer(Launcher *l, const char *name, int on)
{
    int i = game_entry(l, name);
    if (i >= 0) l->steer_mode[i] = !!on;
}

/* Head movement looks around unless set to steer for the game. */
static int game_steer(const Launcher *l, const char *name)
{
    int k = find_game_stereo(l, name);
    return k >= 0 && l->steer_mode[k] == 1;
}

static int game_tracker(const Launcher *l, const char *name)
{
    int i = find_game_stereo(l, name);
    return i >= 0 ? l->tracker_mode[i] : TRACKER_FROM_ZIP;
}

/* A game not yet set: interlaced if its name says so, VFX1 pages otherwise. */
static int game_stereo(const Launcher *l, const char *name)
{
    int k = find_game_stereo(l, name);
    return k >= 0 ? l->stereo_mode[k] : stereo_hint(name);
}

static int stereo_hint(const char *name)
{
    static const char *const lines_hints[] = { "iglass", "i-glass", "maxx", "interlace", "lcd" };
    char lower[LAUNCHER_NAME];
    size_t i;
    for (i = 0; name[i] && i < sizeof lower - 1; i++) lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = 0;
    for (i = 0; i < sizeof lines_hints / sizeof lines_hints[0]; i++)
        if (strstr(lower, lines_hints[i])) return STEREO_LINES;
    return STEREO_PAGES;
}

static int core_option_index(const Launcher *l, const char *key)
{
    int i;
    for (i = 0; i < retrohost_option_count(l->host); i++)
        if (!strcmp(retrohost_option_key(l->host, i), key)) return i;
    return -1;
}

/* The settings and core options for a game, or the defaults for "", into
 * effect. Before the game starts, so a CPU type takes. */
static void apply_game(Launcher *l, const char *name)
{
    int i = name[0] ? find_game_stereo(l, name) : -1, k;
    l->settings = i >= 0 && l->game_has_settings[i] ? l->game_settings[i] : l->defaults;
    if (name[0]) {
        l->settings.stereo = game_stereo(l, name);
        l->settings.tracker = game_tracker(l, name);
        l->settings.steer = game_steer(l, name);
    }
    for (k = 0; k < LAUNCHER_GAME_CORE; k++) {
        const char *v = i >= 0 && l->game_core[i][k][0] ? l->game_core[i][k] : l->core_default[k];
        if (v[0]) retrohost_set_option(l->host, GAME_CORE_KEYS[k], v);
    }
    {
        /* DOSBox Pure emulates the prefetch queue only on its normal core and
         * crashes on another */
        const char *type = retrohost_option_current(l->host, "dosbox_pure_cpu_type");
        if (type && strstr(type, "prefetch")) retrohost_set_option(l->host, "dosbox_pure_cpu_core", "normal");
    }
}

/* A headset setting changed: for the running game, or for the defaults. */
static void store_settings(Launcher *l)
{
    int i;
    if (!l->loaded[0]) { l->defaults = l->settings; return; }
    i = game_entry(l, l->loaded);
    if (i < 0) return;
    l->game_settings[i] = l->settings;
    l->game_has_settings[i] = 1;
}

static void save_config(Launcher *l)
{
    char path[600];
    FILE *f;
    int i;
    config_path(l, path, sizeof path);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "fov=%g\ndistance=%g\nlcd_look=%d\nmouse=%d\nmouse_speed=%g\ntracker_boost=%g\npuck_left=%d\nstereo_detect=%d\nstereo_always=%d\nstereo_threshold=%d\nmode_key=%d\nfake3d=%d\ngame=%s\n",
            l->defaults.fov_deg, l->defaults.distance_m, l->defaults.lcd_look,
            l->defaults.mouse, l->defaults.mouse_speed, l->defaults.tracker_gain, l->defaults.puck_left,
            l->defaults.stereo_auto, l->defaults.stereo_force, l->defaults.stereo_threshold, l->defaults.mode_key,
            l->defaults.fake3d, l->loaded);
    for (i = 0; i < retrohost_option_count(l->host); i++) {
        const char *key = retrohost_option_key(l->host, i);
        const char *cur = retrohost_option_current(l->host, key);
        const char *def = retrohost_option_value(l->host, i, 0);
        int k;
        for (k = 0; k < LAUNCHER_GAME_CORE; k++)        /* not a game's override */
            if (!strcmp(key, GAME_CORE_KEYS[k]) && l->core_default[k][0]) cur = l->core_default[k];
        if (cur && def && strcmp(cur, def)) fprintf(f, "core.%s=%s\n", key, cur);
    }
    for (i = 0; i < l->stereo_count; i++) {
        const LauncherSettings *g = &l->game_settings[i];
        int k;
        fprintf(f, "stereo.%s=%d\n", l->stereo_game[i], l->stereo_mode[i]);
        if (l->tracker_mode[i]) fprintf(f, "tracker.%s=%d\n", l->stereo_game[i], l->tracker_mode[i]);
        if (l->steer_mode[i] != 2) fprintf(f, "steer.%s=%d\n", l->stereo_game[i], l->steer_mode[i]);
        if (l->game_has_settings[i])
            fprintf(f, "gameset.%s=%g,%g,%d,%d,%g,%g,%d,%d,%d,%d,%d,%d\n", l->stereo_game[i], g->fov_deg,
                    g->distance_m, g->lcd_look, g->mouse, g->mouse_speed, g->tracker_gain, g->puck_left,
                    g->stereo_auto, g->stereo_force, g->stereo_threshold, g->mode_key, g->fake3d);
        for (k = 0; k < LAUNCHER_GAME_CORE; k++)
            if (l->game_core[i][k][0]) fprintf(f, "gamecore.%s.%s=%s\n", l->stereo_game[i], GAME_CORE_NAMES[k], l->game_core[i][k]);
    }
    fclose(f);
}

static void load_config(Launcher *l, char *last_game, size_t size)
{
    char path[600], line[512];
    FILE *f;
    config_path(l, path, sizeof path);
    last_game[0] = 0;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl;
        if (!eq) continue;
        *eq = 0;
        if ((nl = strpbrk(eq + 1, "\r\n"))) *nl = 0;
        if (!strcmp(line, "fov")) l->settings.fov_deg = atof(eq + 1);
        else if (!strcmp(line, "distance")) l->settings.distance_m = atof(eq + 1);
        else if (!strncmp(line, "stereo.", 7)) set_game_stereo(l, line + 7, atoi(eq + 1));
        else if (!strncmp(line, "tracker.", 8)) set_game_tracker(l, line + 8, atoi(eq + 1));
        else if (!strncmp(line, "steer.", 6)) set_game_steer(l, line + 6, atoi(eq + 1));
        else if (!strcmp(line, "lcd_look")) l->settings.lcd_look = atoi(eq + 1);
        else if (!strcmp(line, "mouse")) l->settings.mouse = atoi(eq + 1);
        else if (!strcmp(line, "puck_mouse")) l->settings.mouse = atoi(eq + 1) ? MOUSE_PUCK : MOUSE_OFF;
        else if (!strcmp(line, "mouse_speed")) l->settings.mouse_speed = atof(eq + 1);
        else if (!strcmp(line, "tracker_boost")) l->settings.tracker_gain = atof(eq + 1);
        else if (!strcmp(line, "puck_left")) l->settings.puck_left = atoi(eq + 1);
        /* the defaults were stereo_auto and stereo_force before: new games
         * now start fixed on stereo, whatever those said */
        else if (!strcmp(line, "stereo_detect")) l->settings.stereo_auto = atoi(eq + 1);
        else if (!strcmp(line, "stereo_always")) l->settings.stereo_force = atoi(eq + 1);
        else if (!strcmp(line, "mode_key")) l->settings.mode_key = atoi(eq + 1);
        else if (!strcmp(line, "fake3d")) l->settings.fake3d = atoi(eq + 1);
        else if (!strcmp(line, "stereo_threshold")) l->settings.stereo_threshold = atoi(eq + 1);
        else if (!strcmp(line, "game")) snprintf(last_game, size, "%s", eq + 1);
        else if (!strncmp(line, "core.", 5)) retrohost_set_option(l->host, line + 5, eq + 1);
        else if (!strncmp(line, "gameset.", 8)) {
            int i = game_entry(l, line + 8);
            LauncherSettings g = l->defaults;
            if (i >= 0 && sscanf(eq + 1, "%lf,%lf,%d,%d,%lf,%lf,%d,%d,%d,%d,%d,%d", &g.fov_deg, &g.distance_m,
                                 &g.lcd_look, &g.mouse, &g.mouse_speed, &g.tracker_gain, &g.puck_left,
                                 &g.stereo_auto, &g.stereo_force, &g.stereo_threshold, &g.mode_key, &g.fake3d) >= 10) {
                if (g.mouse < 0 || g.mouse >= MOUSE_MODES) g.mouse = MOUSE_OFF;
                if (g.mode_key < 0 || g.mode_key >= MODEKEY_COUNT) g.mode_key = MODEKEY_F9_OR_Y;
                if (g.fake3d < 0 || g.fake3d >= FAKE3D_LEVELS) g.fake3d = FAKE3D_OFF;
                if (g.stereo_threshold < 1 || g.stereo_threshold > 5) g.stereo_threshold = 3;
                l->game_settings[i] = g;
                l->game_has_settings[i] = 1;
            }
        } else if (!strncmp(line, "gamecore.", 9)) {
            char *dot = strrchr(line + 9, '.');           /* game names have dots; option names do not */
            int k;
            if (dot) {
                *dot = 0;
                for (k = 0; k < LAUNCHER_GAME_CORE; k++)
                    if (!strcmp(dot + 1, GAME_CORE_NAMES[k])) {
                        int i = game_entry(l, line + 9);
                        if (i >= 0) snprintf(l->game_core[i][k], sizeof l->game_core[i][k], "%s", eq + 1);
                    }
            }
        } else continue;
        /* a default setting: into the defaults too */
        if (strncmp(line, "stereo.", 7) && strncmp(line, "tracker.", 8) && strncmp(line, "steer.", 6) &&
            strncmp(line, "core.", 5) && strcmp(line, "game"))
            l->defaults = l->settings;
    }
    fclose(f);
    if (l->settings.mouse < 0 || l->settings.mouse >= MOUSE_MODES) l->settings.mouse = MOUSE_OFF;
    if (l->settings.mouse_speed <= 0) l->settings.mouse_speed = 1;
    if (l->settings.tracker_gain <= 0) l->settings.tracker_gain = 1;
    if (l->settings.stereo_threshold < 1 || l->settings.stereo_threshold > 5) l->settings.stereo_threshold = 3;
    if (l->settings.mode_key < 0 || l->settings.mode_key >= MODEKEY_COUNT) l->settings.mode_key = MODEKEY_F9_OR_Y;
    if (l->settings.fake3d < 0 || l->settings.fake3d >= FAKE3D_LEVELS) l->settings.fake3d = FAKE3D_OFF;
    l->defaults = l->settings;
    {
        /* the core options games may override, as every other game has them */
        int k;
        for (k = 0; k < LAUNCHER_GAME_CORE; k++) {
            const char *v = retrohost_option_current(l->host, GAME_CORE_KEYS[k]);
            snprintf(l->core_default[k], sizeof l->core_default[k], "%s", v ? v : "");
        }
    }
}

/* ------------------------------------------------------------ games */

static int is_game(const char *name)
{
    size_t n = strlen(name);
    if (n < 5 || strcasecmp(name + n - 4, ".zip")) return 0;
    if (n >= 9 && !strcasecmp(name + n - 9, ".pure.zip")) return 0;     /* DOSBox Pure saves */
    return 1;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void scan_games(Launcher *l)
{
    DIR *d = opendir(l->files_dir);
    struct dirent *e;
    l->game_count = 0;
    if (!d) return;
    while ((e = readdir(d)) && l->game_count < LAUNCHER_MAX_GAMES)
        if (is_game(e->d_name)) snprintf(l->games[l->game_count++], LAUNCHER_NAME, "%s", e->d_name);
    closedir(d);
    qsort(l->games, (size_t)l->game_count, LAUNCHER_NAME, by_name);
}

static void set_status(Launcher *l, const char *fmt, const char *arg)
{
    pthread_mutex_lock(&l->lock);
    snprintf(l->status, sizeof l->status, fmt, arg ? arg : "");
    pthread_mutex_unlock(&l->lock);
}

static int load_game(Launcher *l, const char *name)
{
    char path[700];
    snprintf(path, sizeof path, "%s/%s", l->files_dir, name);
    retrohost_unload(l->host);
    l->loaded[0] = 0;
    apply_game(l, name);
    {
        /* the game's serial head tracker, if chosen here, before the machine starts */
        int t = game_tracker(l, name), port;
        for (port = 1; port <= 4; port++) retrohost_vfx1_serial_tracker(l->host, port, -1);
        if (t == TRACKER_IGLASSES_COM1) retrohost_vfx1_serial_tracker(l->host, 1, 0);
        if (t == TRACKER_IGLASSES_COM2) retrohost_vfx1_serial_tracker(l->host, 2, 0);
        if (t == TRACKER_CYBERMAXX_COM1) retrohost_vfx1_serial_tracker(l->host, 1, 1);
        if (t == TRACKER_CYBERMAXX_COM2) retrohost_vfx1_serial_tracker(l->host, 2, 1);
    }
    if (!retrohost_load(l->host, path)) {
        set_status(l, "Could not load %s", name);
        return 0;
    }
    snprintf(l->loaded, sizeof l->loaded, "%s", name);
    l->just_loaded = 1;
    set_status(l, "Running %s", name);
    save_config(l);
    return 1;
}

/* ------------------------------------------------------------ server */

static void *list_server(void *arg)
{
    Launcher *l = (Launcher *)arg;
    static char page[1 << 16];
    char err[256];
    int n = 0;
    if (http_get_text(l->server_dir, page, sizeof page, err, sizeof err)) {
        const char *p = page;
        pthread_mutex_lock(&l->lock);
        while ((p = strstr(p, "href=\"")) && n < LAUNCHER_MAX_GAMES) {
            const char *q;
            p += 6;
            q = strchr(p, '"');
            if (!q) break;
            if ((size_t)(q - p) < LAUNCHER_NAME) {
                char name[LAUNCHER_NAME];
                snprintf(name, sizeof name, "%.*s", (int)(q - p), p);
                if (is_game(name) && !strchr(name, '/')) snprintf(l->server_games[n++], LAUNCHER_NAME, "%s", name);
            }
            p = q;
        }
        l->server_count = n;
        l->server_listed = 1;
        snprintf(l->status, sizeof l->status, "%d game(s) on the PC", n);
        pthread_mutex_unlock(&l->lock);
    } else {
        set_status(l, "PC not reachable: %s", err);
    }
    pthread_mutex_lock(&l->lock);
    l->worker_busy = 0;
    pthread_mutex_unlock(&l->lock);
    return NULL;
}

typedef struct DownloadJob { Launcher *l; char name[LAUNCHER_NAME]; } DownloadJob;

static void *download_game(void *arg)
{
    DownloadJob *job = (DownloadJob *)arg;
    Launcher *l = job->l;
    char url[800], dest[700], err[256];
    snprintf(url, sizeof url, "%s%s", l->server_dir, job->name);
    snprintf(dest, sizeof dest, "%s/%s", l->files_dir, job->name);
    set_status(l, "Downloading %s ...", job->name);
    if (http_download(url, dest, err, sizeof err)) set_status(l, "Downloaded %s", job->name);
    else set_status(l, "Download failed: %s", err);
    pthread_mutex_lock(&l->lock);
    l->worker_busy = 0;
    pthread_mutex_unlock(&l->lock);
    free(job);
    return NULL;
}

static void start_worker(Launcher *l, void *(*fn)(void *), void *arg)
{
    pthread_mutex_lock(&l->lock);
    if (l->worker_busy) { pthread_mutex_unlock(&l->lock); free(arg == l ? NULL : arg); return; }
    l->worker_busy = 1;
    pthread_mutex_unlock(&l->lock);
    if (pthread_create(&l->worker, NULL, fn, arg) == 0) pthread_detach(l->worker);
    else { l->worker_busy = 0; if (arg != l) free(arg); }
}

/* ------------------------------------------------------------ pages */

static void add_choice_d(VfxMenu *m, int id, const char *label, const char *fmt, double v)
{
    char value[32];
    snprintf(value, sizeof value, fmt, v);
    vfxmenu_add(m, VFXMENU_CHOICE, id, label, value);
}

static void build(Launcher *l)
{
    VfxMenu *m = &l->menu;
    char buf[VFXMENU_COLS + 1];
    int i;

    switch (l->page) {
    case PAGE_MAIN:
        vfxmenu_begin(m, "VFX1 Quest  -  DOSBox Pure with a Forte VFX1", FOOTER);
        if (l->loaded[0]) {
            snprintf(buf, sizeof buf, "Resume %s", l->loaded);
            vfxmenu_add(m, VFXMENU_ACTION, ID_RESUME, buf, NULL);
        }
        vfxmenu_add(m, VFXMENU_ACTION, ID_GAMES, "Load a game...", NULL);
        if (l->server_dir[0]) vfxmenu_add(m, VFXMENU_ACTION, ID_DOWNLOADS, "Download games from the PC...", NULL);
        vfxmenu_add(m, VFXMENU_ACTION, ID_CORE, "DOSBox Pure options...", NULL);
        vfxmenu_add(m, VFXMENU_ACTION, ID_VFX1, "VFX1 headset settings...", NULL);
        if (l->loaded[0]) vfxmenu_add(m, VFXMENU_ACTION, ID_UNLOAD, "Close the game", NULL);
        vfxmenu_add(m, VFXMENU_ACTION, ID_QUIT, "Quit VFX1 Quest", NULL);
        break;

    case PAGE_GAMES:
        scan_games(l);
        vfxmenu_begin(m, "Load a game", FOOTER);
        snprintf(buf, sizeof buf, "Zip files in %s", l->files_dir);
        vfxmenu_add(m, VFXMENU_HEADING, 0, buf, NULL);
        for (i = 0; i < l->game_count; i++)
            vfxmenu_add(m, VFXMENU_ACTION, ID_GAME_BASE + i, l->games[i],
                        !strcmp(l->games[i], l->loaded) ? "running" : NULL);
        if (!l->game_count) vfxmenu_add(m, VFXMENU_HEADING, 0, "  No games yet: download some from the PC, or push zips with adb.", NULL);
        vfxmenu_add(m, VFXMENU_ACTION, ID_BACK, "Back", NULL);
        break;

    case PAGE_DOWNLOAD:
        scan_games(l);
        vfxmenu_begin(m, "Download games from the PC", FOOTER);
        snprintf(buf, sizeof buf, "From %s", l->server_dir);
        vfxmenu_add(m, VFXMENU_HEADING, 0, buf, NULL);
        pthread_mutex_lock(&l->lock);
        for (i = 0; i < l->server_count; i++) {
            int have = 0, g;
            for (g = 0; g < l->game_count; g++) if (!strcmp(l->games[g], l->server_games[i])) have = 1;
            vfxmenu_add(m, VFXMENU_ACTION, ID_SERVER_BASE + i, l->server_games[i], have ? "on headset" : "download");
        }
        pthread_mutex_unlock(&l->lock);
        vfxmenu_add(m, VFXMENU_ACTION, ID_REFRESH, "Refresh the list", NULL);
        vfxmenu_add(m, VFXMENU_ACTION, ID_BACK, "Back", NULL);
        break;

    case PAGE_CORE: {
        char last_group[64] = "";
        vfxmenu_begin(m, "DOSBox Pure options  (some apply after the game is reloaded)", FOOTER);
        for (i = 0; i < retrohost_option_count(l->host); i++) {
            const char *label = retrohost_option_label(l->host, i);
            const char *key = retrohost_option_key(l->host, i);
            const char *sep = strstr(label, " > ");
            char group[64];
            const char *name = label;
            if (sep) {
                snprintf(group, sizeof group, "%.*s", (int)(sep - label), label);
                name = sep + 3;
                if (strcmp(group, last_group)) {
                    vfxmenu_add(m, VFXMENU_HEADING, 0, group, NULL);
                    snprintf(last_group, sizeof last_group, "%s", group);
                }
            }
            vfxmenu_add(m, VFXMENU_CHOICE, ID_OPTION_BASE + i, name, retrohost_option_current(l->host, key));
        }
        vfxmenu_add(m, VFXMENU_ACTION, ID_BACK, "Back", NULL);
        break;
    }

    case PAGE_VFX1: {
        int g = l->loaded[0] ? find_game_stereo(l, l->loaded) : -1, k;
        if (l->loaded[0]) snprintf(buf, sizeof buf, "VFX1 headset settings for %s", l->loaded);
        else snprintf(buf, sizeof buf, "VFX1 headset settings: the defaults for every game");
        vfxmenu_begin(m, buf, FOOTER);
        if (l->loaded[0])
            vfxmenu_add(m, VFXMENU_HEADING, 0, g >= 0 && l->game_has_settings[g] ? "This game has its own settings."
                                                 : "This game uses the defaults until one is changed here.", NULL);
        vfxmenu_add(m, VFXMENU_HEADING, 0, "Display", NULL);
        add_choice_d(m, ID_FOV, "Screen width (the real VFX1: 35.5)", "%g degrees", l->settings.fov_deg);
        add_choice_d(m, ID_DISTANCE, "Screen distance (the real VFX1: 3.35)", "%g m", l->settings.distance_m);
        if (l->loaded[0])
            vfxmenu_add(m, VFXMENU_CHOICE, ID_STEREO, "Stereo format", STEREO_NAMES[l->settings.stereo]);
        vfxmenu_add(m, VFXMENU_CHOICE, ID_STEREO_AUTO, "Stereo or flat screen",
                    l->settings.stereo_auto ? "detected (experimental)" : "fixed");
        if (l->settings.stereo_auto)
            vfxmenu_add(m, VFXMENU_CHOICE, ID_STEREO_THRESHOLD, "  Flat-screen threshold",
                        THRESHOLD_NAMES[l->settings.stereo_threshold - 1]);
        else
            vfxmenu_add(m, VFXMENU_CHOICE, ID_STEREO_FORCE, "  Always show", l->settings.stereo_force ? "stereo" : "flat");
        vfxmenu_add(m, VFXMENU_CHOICE, ID_MODE_KEY, "  Mode change keymap", MODE_KEY_NAMES[l->settings.mode_key]);
        vfxmenu_add(m, VFXMENU_CHOICE, ID_FAKE3D, "Fake 3D for flat screens (experimental)",
                    FAKE3D_NAMES[l->settings.fake3d]);
        vfxmenu_add(m, VFXMENU_CHOICE, ID_LCD, "VFX1 LCD look (dotted panel)", l->settings.lcd_look ? "on" : "off");
        vfxmenu_add(m, VFXMENU_HEADING, 0, "CyberPuck", NULL);
        vfxmenu_add(m, VFXMENU_CHOICE, ID_PUCK_HAND, "Controller used as the CyberPuck", l->settings.puck_left ? "left" : "right");
        vfxmenu_add(m, VFXMENU_HEADING, 0, "DOS mouse (trigger: left button, grip: right button)", NULL);
        vfxmenu_add(m, VFXMENU_CHOICE, ID_MOUSE, "Mouse", MOUSE_NAMES[l->settings.mouse]);
        add_choice_d(m, ID_MOUSE_SPEED, "Mouse speed", "%gx", l->settings.mouse_speed);
        if (l->loaded[0]) {
            vfxmenu_add(m, VFXMENU_HEADING, 0, "Head tracking", NULL);
            vfxmenu_add(m, VFXMENU_CHOICE, ID_STEER, "Head movement", l->settings.steer ? "steers" : "looks (1:1)");
        }
        vfxmenu_add(m, VFXMENU_HEADING, 0, "i-glasses! and CyberMaxx", NULL);
        if (l->loaded[0])
            vfxmenu_add(m, VFXMENU_CHOICE, ID_TRACKER_PORT, "Head tracker on a COM port (reloads)", TRACKER_NAMES[l->settings.tracker]);
        add_choice_d(m, ID_TRACKER_GAIN, "Extra head tracker gain", "%gx", l->settings.tracker_gain);
        if (l->loaded[0]) {
            vfxmenu_add(m, VFXMENU_HEADING, 0, "DOSBox Pure, this game only", NULL);
            for (k = 0; k < LAUNCHER_GAME_CORE; k++) {
                char value[48];
                if (g >= 0 && l->game_core[g][k][0]) snprintf(value, sizeof value, "%s", l->game_core[g][k]);
                else snprintf(value, sizeof value, "as the defaults (%s)", l->core_default[k]);
                vfxmenu_add(m, VFXMENU_CHOICE, ID_GAME_CORE_BASE + k, GAME_CORE_LABELS[k], value);
            }
            vfxmenu_add(m, VFXMENU_ACTION, ID_RESET_GAME, "Reset this game to the defaults", NULL);
        }
        vfxmenu_add(m, VFXMENU_ACTION, ID_BACK, "Back", NULL);
        break;
    }
    }
    pthread_mutex_lock(&l->lock);
    vfxmenu_set_status(m, l->status);
    pthread_mutex_unlock(&l->lock);
}

static void go(Launcher *l, LauncherPage page)
{
    l->page_selected[l->page] = l->menu.selected;
    l->page = page;
    build(l);
    vfxmenu_select(&l->menu, l->page_selected[page]);
}

static int step_index(double current, const double *list, int n, int dir)
{
    int i, best = 0;
    for (i = 0; i < n; i++)
        if (fabs(list[i] - current) < fabs(list[best] - current)) best = i;
    best += dir;
    return best < 0 ? 0 : best >= n ? n - 1 : best;
}

/* ------------------------------------------------------------ public */

void launcher_init(Launcher *l, RetroHost *host, const char *files_dir, const char *server_dir)
{
    memset(l, 0, sizeof *l);
    pthread_mutex_init(&l->lock, NULL);
    l->host = host;
    snprintf(l->files_dir, sizeof l->files_dir, "%s", files_dir);
    snprintf(l->server_dir, sizeof l->server_dir, "%s", server_dir ? server_dir : "");
    l->settings.fov_deg = 35.5;
    l->settings.distance_m = 3.35;
    l->settings.stereo = 1;
    l->settings.mouse_speed = 1;
    l->settings.tracker_gain = 1;
    l->settings.stereo_auto = 0;
    l->settings.stereo_force = 1;
    l->settings.stereo_threshold = 3;
    l->settings.mode_key = MODEKEY_F9_OR_Y;
    l->defaults = l->settings;
    l->page = PAGE_MAIN;
    l->open = 1;
    build(l);
    vfxmenu_select(&l->menu, 0);
}

int launcher_autostart(Launcher *l)
{
    char last[LAUNCHER_NAME];
    char starting[600];
    FILE *f;
    load_config(l, last, sizeof last);
    scan_games(l);
    starting_path(l, starting, sizeof starting);
    if ((f = fopen(starting, "r"))) {
        fclose(f);
        remove(starting);
        set_status(l, "%s crashed last time it started, so it was not resumed", last);
        last[0] = 0;
    }
    if (last[0]) {
        int i;
        for (i = 0; i < l->game_count; i++)
            if (!strcmp(l->games[i], last)) {
                if ((f = fopen(starting, "w"))) fclose(f);
                if (load_game(l, last)) { l->open = 0; l->starting = 1; return 1; }
                remove(starting);
            }
    }
    if (l->game_count == 0 && l->server_dir[0]) {
        l->page = PAGE_DOWNLOAD;
        start_worker(l, list_server, l);
    }
    build(l);
    vfxmenu_select(&l->menu, 0);
    return 0;
}

void launcher_started(Launcher *l)
{
    char starting[600];
    if (!l->starting) return;
    l->starting = 0;
    starting_path(l, starting, sizeof starting);
    remove(starting);
}

void launcher_set_open(Launcher *l, int open)
{
    l->open = open;
    if (open) {
        l->page = PAGE_MAIN;
        build(l);
        vfxmenu_select(&l->menu, 0);
    }
}

void launcher_update(Launcher *l, const VfxMenuInput *in)
{
    VfxMenuEvent ev = vfxmenu_input(&l->menu, in);
    int dir = ev.type == VFXMENU_EVENT_LEFT ? -1 : 1;
    int changed = 0;

    l->just_loaded = 0;

    if (ev.type == VFXMENU_EVENT_BACK) {
        if (l->page == PAGE_MAIN) { if (l->loaded[0]) l->open = 0; }
        else go(l, PAGE_MAIN);
    } else if (ev.type != VFXMENU_EVENT_NONE) {
        int id = ev.id;
        if (id == ID_BACK) go(l, PAGE_MAIN);
        else if (id == ID_RESUME) l->open = 0;
        else if (id == ID_GAMES) go(l, PAGE_GAMES);
        else if (id == ID_DOWNLOADS) {
            go(l, PAGE_DOWNLOAD);
            if (!l->server_listed) start_worker(l, list_server, l);
        } else if (id == ID_REFRESH) start_worker(l, list_server, l);
        else if (id == ID_CORE) go(l, PAGE_CORE);
        else if (id == ID_VFX1) go(l, PAGE_VFX1);
        else if (id == ID_UNLOAD) {
            retrohost_unload(l->host);
            l->loaded[0] = 0;
            apply_game(l, "");
            save_config(l);
            set_status(l, "Game closed", NULL);
        } else if (id == ID_QUIT) l->quit_requested = 1;
        else if (id >= ID_GAME_BASE && id < ID_GAME_BASE + LAUNCHER_MAX_GAMES && ev.type == VFXMENU_EVENT_ACTIVATE) {
            if (load_game(l, l->games[id - ID_GAME_BASE])) l->open = 0;
        } else if (id >= ID_SERVER_BASE && id < ID_SERVER_BASE + LAUNCHER_MAX_GAMES) {
            DownloadJob *job = (DownloadJob *)calloc(1, sizeof *job);
            if (job) {
                job->l = l;
                pthread_mutex_lock(&l->lock);
                snprintf(job->name, sizeof job->name, "%s", l->server_games[id - ID_SERVER_BASE]);
                pthread_mutex_unlock(&l->lock);
                start_worker(l, download_game, job);
            }
        } else if (id >= ID_OPTION_BASE) {
            int index = id - ID_OPTION_BASE, n = retrohost_option_value_count(l->host, index), v, cur = 0;
            const char *key = retrohost_option_key(l->host, index);
            const char *now = retrohost_option_current(l->host, key);
            for (v = 0; v < n; v++) if (now && !strcmp(retrohost_option_value(l->host, index, v), now)) cur = v;
            if (n > 0) {
                int k, g = l->loaded[0] ? find_game_stereo(l, l->loaded) : -1;
                cur = (cur + dir + n) % n;
                retrohost_set_option(l->host, key, retrohost_option_value(l->host, index, cur));
                for (k = 0; k < LAUNCHER_GAME_CORE; k++)
                    if (!strcmp(key, GAME_CORE_KEYS[k])) {
                        snprintf(l->core_default[k], sizeof l->core_default[k], "%s", retrohost_option_value(l->host, index, cur));
                        if (g >= 0 && l->game_core[g][k][0]) {
                            retrohost_set_option(l->host, key, l->game_core[g][k]);
                            set_status(l, "The running game has its own value (VFX1 headset settings)", NULL);
                        }
                    }
                changed = 1;
            }
        } else if (id >= ID_GAME_CORE_BASE && id < ID_GAME_CORE_BASE + LAUNCHER_GAME_CORE && l->loaded[0]) {
            /* "" (the defaults), then each of the option's values */
            int k = id - ID_GAME_CORE_BASE, g = game_entry(l, l->loaded), index = core_option_index(l, GAME_CORE_KEYS[k]);
            int n = index >= 0 ? retrohost_option_value_count(l->host, index) : 0, cur = 0, v;
            if (g >= 0 && n > 0) {
                for (v = 0; v < n; v++)
                    if (!strcmp(retrohost_option_value(l->host, index, v), l->game_core[g][k])) cur = v + 1;
                cur = (cur + dir + n + 1) % (n + 1);
                snprintf(l->game_core[g][k], sizeof l->game_core[g][k], "%s",
                         cur ? retrohost_option_value(l->host, index, cur - 1) : "");
                retrohost_set_option(l->host, GAME_CORE_KEYS[k], cur ? l->game_core[g][k] : l->core_default[k]);
                changed = 1;
                if (GAME_CORE_RESTART[k]) {
                    char name[LAUNCHER_NAME];
                    save_config(l);
                    snprintf(name, sizeof name, "%s", l->loaded);
                    load_game(l, name);
                }
            }
        } else if (id == ID_RESET_GAME && l->loaded[0] && ev.type == VFXMENU_EVENT_ACTIVATE) {
            int g = game_entry(l, l->loaded);
            if (g >= 0) {
                memset(l->game_core[g], 0, sizeof l->game_core[g]);
                l->game_has_settings[g] = 0;
                apply_game(l, l->loaded);
                set_status(l, "%s now uses the defaults", l->loaded);
                save_config(l);
            }
        } else if (id == ID_FOV) {
            l->settings.fov_deg = FOVS[step_index(l->settings.fov_deg, FOVS, 5, dir)];
            changed = 1;
        } else if (id == ID_DISTANCE) {
            l->settings.distance_m = DISTANCES[step_index(l->settings.distance_m, DISTANCES, 4, dir)];
            changed = 1;
        } else if (id == ID_STEREO) {
            l->settings.stereo = (l->settings.stereo + dir + STEREO_MODES) % STEREO_MODES;
            set_game_stereo(l, l->loaded, l->settings.stereo);
            changed = 1;
        }
        else if (id == ID_STEREO_AUTO) { l->settings.stereo_auto = !l->settings.stereo_auto; changed = 1; }
        else if (id == ID_STEREO_FORCE) { l->settings.stereo_force = !l->settings.stereo_force; changed = 1; }
        else if (id == ID_FAKE3D) {
            l->settings.fake3d = (l->settings.fake3d + dir + FAKE3D_LEVELS) % FAKE3D_LEVELS;
            changed = 1;
        }
        else if (id == ID_MODE_KEY) {
            l->settings.mode_key = (l->settings.mode_key + dir + MODEKEY_COUNT) % MODEKEY_COUNT;
            changed = 1;
        }
        else if (id == ID_STEREO_THRESHOLD) {
            int t = l->settings.stereo_threshold + dir;
            l->settings.stereo_threshold = t < 1 ? 1 : t > 5 ? 5 : t;
            changed = 1;
        }
        else if (id == ID_LCD) { l->settings.lcd_look = !l->settings.lcd_look; changed = 1; }
        else if (id == ID_MOUSE) { l->settings.mouse = (l->settings.mouse + dir + MOUSE_MODES) % MOUSE_MODES; changed = 1; }
        else if (id == ID_STEER) {
            l->settings.steer = !l->settings.steer;
            set_game_steer(l, l->loaded, l->settings.steer);
            changed = 1;
        }
        else if (id == ID_TRACKER_PORT) {
            l->settings.tracker = (l->settings.tracker + dir + TRACKER_MODES) % TRACKER_MODES;
            set_game_tracker(l, l->loaded, l->settings.tracker);
            changed = 1;
            if (l->loaded[0]) {                 /* ports exist from machine start: restart the game */
                char name[LAUNCHER_NAME];
                snprintf(name, sizeof name, "%s", l->loaded);
                load_game(l, name);
            }
        }
        else if (id == ID_TRACKER_GAIN) {
            l->settings.tracker_gain = TRACKER_GAINS[step_index(l->settings.tracker_gain, TRACKER_GAINS, 6, dir)];
            changed = 1;
        }
        else if (id == ID_MOUSE_SPEED) {
            l->settings.mouse_speed = MOUSE_SPEEDS[step_index(l->settings.mouse_speed, MOUSE_SPEEDS, 5, dir)];
            changed = 1;
        }
        else if (id == ID_PUCK_HAND) { l->settings.puck_left = !l->settings.puck_left; changed = 1; }
    }
    if (changed && l->page == PAGE_VFX1) store_settings(l);
    if (changed) save_config(l);

    /* rebuild to show new values and background progress, keeping the place */
    {
        int sel = l->menu.selected, scroll = l->menu.scroll;
        build(l);
        l->menu.selected = sel < l->menu.count ? sel : l->menu.count - 1;
        l->menu.scroll = scroll;
        vfxmenu_select(&l->menu, l->menu.selected);
    }
}

int launcher_step_view_mode(Launcher *l)
{
    int mode = l->settings.stereo_auto ? VIEW_AUTO : l->settings.stereo_force ? VIEW_STEREO : VIEW_FLAT;
    mode = (mode + 1) % 3;
    l->settings.stereo_auto = mode == VIEW_AUTO;
    if (mode != VIEW_AUTO) l->settings.stereo_force = mode == VIEW_STEREO;
    store_settings(l);
    save_config(l);
    build(l);
    return mode;
}
