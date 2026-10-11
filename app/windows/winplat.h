/*
 * winplat.h -- what both Windows apps (vfx1xr.exe and the desktop debug
 * app vfx1desk.exe) need from Windows besides drawing: paths, the log,
 * crash reports and sound.
 */
#ifndef VFX_WINPLAT_H
#define VFX_WINPLAT_H

#include <windows.h>
#include "vfx/vfxapp.h"

typedef struct WinPaths {
    char dir[MAX_PATH];         /* the program's folder */
    char games[MAX_PATH];       /* argv[1], or games\ in dir */
    char core[MAX_PATH];        /* dosbox_pure_libretro.dll in dir */
    char temp[MAX_PATH];
} WinPaths;

/* Paths, then the log on the console and in <dir>\<log_name>, the
 * VFX1_LOG_URL forwarding and the crash handler. */
void winplat_start(WinPaths *p, int argc, char **argv, const char *log_name);
/* The launcher's server listing: VFX1_SERVER, else the built-in one. */
const char *winplat_server(const char *builtin);
double winplat_now(void);

/* Sound from vfxapp's buffer on its own thread. */
void winplat_audio_start(VfxApp *vfx, int rate);
void winplat_audio_stop(void);

void winplat_stop(void);

/* A Windows key, as WM_KEYDOWN gives it (`lp` for the scan code and the
 * extended bit), as a libretro DOS key; RETROK_UNKNOWN if it has none. */
unsigned winplat_retrok_from_vk(WPARAM vk, LPARAM lp);

#endif
