/*
 * launcher.h -- the in-headset menu for choosing a game and configuring the
 * core and the VFX1 display.
 *
 * Built on vfxmenu (drawing, navigation) and retrohost (the core, its options,
 * loading and unloading content). It knows nothing about OpenXR or OpenGL:
 * the host feeds it navigation input, draws its menu buffer, and reads its
 * settings, so a later virtual room can reuse it unchanged.
 *
 * Settings and chosen core options persist in <files>/vfx1quest.cfg.
 *
 * The VFX1 headset settings are per game: while a game runs, changing one
 * changes it for that game; with no game, for the defaults every game
 * starts from. A game can also override a few DOSBox Pure options (cycles,
 * CPU type and core), so the global options page stays as it was for every
 * other game.
 */
#ifndef VFX1QUEST_LAUNCHER_H
#define VFX1QUEST_LAUNCHER_H

#include <pthread.h>
#include "vfx/retrohost.h"
#include "vfx/vfxmenu.h"

#define LAUNCHER_MAX_GAMES 256
#define LAUNCHER_NAME 128
#define LAUNCHER_GAME_CORE 3        /* core options a game can override */

typedef struct LauncherSettings {
    double fov_deg;         /* display width */
    double distance_m;      /* display distance */
    int stereo;             /* LauncherStereo for the running game */
    int tracker;            /* LauncherTracker for the running game */
    int steer;              /* 1: the running game steers by head movement */
    int lcd_look;           /* 1: draw the screen as the VFX1's dotted LCDs */
    int mouse;              /* LauncherMouse: what drives the DOS mouse */
    double mouse_speed;     /* multiplier on the mouse rate */
    double tracker_gain;    /* extra, on the i-glasses and CyberMaxx head trackers */
    int puck_left;          /* 1: the left controller is the CyberPuck */
    int stereo_auto;        /* 1: tell stereo from flat screens by the picture */
    int stereo_force;       /* with stereo_auto off: 1 always stereo, 0 always flat */
    int stereo_threshold;   /* with stereo_auto on: 1 stays stereo .. 5 goes flat sooner, 3 default */
    int mode_key;           /* LauncherModeKey: steps detected, stereo, flat while playing */
    int fake3d;             /* LauncherFake3D: depth-model stereo for screens shown flat */
} LauncherSettings;

/* Fake 3D (src/vfxdepth) for screens shown flat: off or a strength, the
 * pixels between the eyes for the nearest point at 320 wide. */
typedef enum LauncherFake3D { FAKE3D_OFF, FAKE3D_SUBTLE, FAKE3D_MEDIUM, FAKE3D_STRONG, FAKE3D_LEVELS } LauncherFake3D;

/* The key or button that steps the stereo-or-flat mode while a game runs.
 * It is kept from the game. */
typedef enum LauncherModeKey {
    MODEKEY_NONE, MODEKEY_F9_OR_Y, MODEKEY_SCROLL_OR_STICK, MODEKEY_PAUSE, MODEKEY_COUNT
} LauncherModeKey;

/* What the mode key steps through. */
typedef enum LauncherViewMode { VIEW_AUTO, VIEW_STEREO, VIEW_FLAT } LauncherViewMode;

/* How the running game draws its two eyes. Remembered per game. */
typedef enum LauncherStereo {
    STEREO_OFF,
    STEREO_PAGES,           /* VFX1: pages in a 640x480 frame */
    STEREO_LINES,           /* i-glasses!, CyberMaxx, Quake lcd_x: interlaced lines */
    STEREO_MODES
} LauncherStereo;

/* An emulated serial head tracker for the running game, if its zip does not
 * set one up. Remembered per game. */
typedef enum LauncherTracker {
    TRACKER_FROM_ZIP, TRACKER_IGLASSES_COM1, TRACKER_IGLASSES_COM2,
    TRACKER_CYBERMAXX_COM1, TRACKER_CYBERMAXX_COM2, TRACKER_MODES
} LauncherTracker;

typedef enum LauncherMouse {
    MOUSE_OFF, MOUSE_PUCK, MOUSE_POINT_RIGHT, MOUSE_POINT_LEFT, MOUSE_MODES
} LauncherMouse;

typedef enum LauncherPage {
    PAGE_MAIN, PAGE_GAMES, PAGE_DOWNLOAD, PAGE_CORE, PAGE_VFX1
} LauncherPage;

typedef struct Launcher {
    VfxMenu menu;
    LauncherPage page;
    int page_selected[5];
    int open;

    RetroHost *host;
    char files_dir[512];
    char server_dir[512];           /* http://host:port/native/ or empty */
    char loaded[LAUNCHER_NAME];     /* file name of the running game, or empty */
    int just_loaded;                /* 1 for one update after a game loads */
    int quit_requested;
    int starting;                   /* a resumed game not yet past its start-up */

    char games[LAUNCHER_MAX_GAMES][LAUNCHER_NAME];
    int game_count;

    char stereo_game[LAUNCHER_MAX_GAMES][LAUNCHER_NAME];    /* per-game stereo choices */
    int stereo_mode[LAUNCHER_MAX_GAMES];
    int stereo_count;
    int tracker_mode[LAUNCHER_MAX_GAMES];   /* LauncherTracker, alongside stereo_game */
    int steer_mode[LAUNCHER_MAX_GAMES];     /* 0 look, 1 steer, 2 not set */
    LauncherSettings game_settings[LAUNCHER_MAX_GAMES];    /* a game's own settings, alongside stereo_game */
    unsigned char game_has_settings[LAUNCHER_MAX_GAMES];
    char game_core[LAUNCHER_MAX_GAMES][LAUNCHER_GAME_CORE][32];    /* "" = the default */
    char core_default[LAUNCHER_GAME_CORE][32];                     /* those options for every other game */
    LauncherSettings defaults;      /* with no game, and for a game not set */

    pthread_mutex_t lock;           /* guards the server list and status */
    pthread_t worker;
    int worker_busy;
    char server_games[LAUNCHER_MAX_GAMES][LAUNCHER_NAME];
    int server_count, server_listed;
    char status[VFXMENU_COLS + 1];

    LauncherSettings settings;      /* in effect now: the running game's, or the defaults */
} Launcher;

void launcher_init(Launcher *l, RetroHost *host, const char *files_dir, const char *server_dir);
/* Loads the previously played game, if any; returns 1 if a game is running. */
int launcher_autostart(Launcher *l);
/* One frame of menu input; rebuilds the page. Only while open. */
void launcher_update(Launcher *l, const VfxMenuInput *in);
void launcher_set_open(Launcher *l, int open);
/* The resumed game has run a while without crashing: resume it next time too. */
void launcher_started(Launcher *l);
/* The mode key: the running game's stereo-or-flat mode steps to the next
 * one (detected, always stereo, always flat), saved like a menu change.
 * Returns the new LauncherViewMode. */
int launcher_step_view_mode(Launcher *l);

#endif
