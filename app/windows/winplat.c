/*
 * winplat.c -- see winplat.h.
 */
#define WIN32_LEAN_AND_MEAN
#include "winplat.h"

#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vfx/applog.h"
#include "libretro.h"

#define TAG "VFX1Win"

static FILE *g_logfile;
static CRITICAL_SECTION g_loglock;

/* ------------------------------------------------------------ log */

static void log_sink(int prio, const char *tag, const char *line)
{
    const char *p = prio >= APPLOG_ERROR ? "E" : prio == APPLOG_WARN ? "W" : "I";
    EnterCriticalSection(&g_loglock);
    fprintf(stderr, "%s %s: %s\n", p, tag, line);
    if (g_logfile) { fprintf(g_logfile, "%s %s: %s\n", p, tag, line); fflush(g_logfile); }
    LeaveCriticalSection(&g_loglock);
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *e)
{
    char msg[600], module[MAX_PATH] = "?";
    void *addr = e->ExceptionRecord->ExceptionAddress;
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m))
        GetModuleFileNameA(m, module, sizeof module);
    snprintf(msg, sizeof msg, "crash: exception 0x%08lx at %s+0x%llx", e->ExceptionRecord->ExceptionCode, module,
             (unsigned long long)((char *)addr - (char *)m));
    applog(APPLOG_FATAL, TAG, "%s", msg);
    applog_post_now(msg);
    return EXCEPTION_CONTINUE_SEARCH;
}

void winplat_start(WinPaths *p, int argc, char **argv, const char *log_name)
{
    char logpath[MAX_PATH];
    char *slash;

    GetModuleFileNameA(NULL, p->dir, sizeof p->dir);
    if ((slash = strrchr(p->dir, '\\'))) *slash = 0;
    snprintf(p->games, sizeof p->games, "%s", argc > 1 ? argv[1] : "");
    if (!p->games[0]) snprintf(p->games, sizeof p->games, "%s\\games", p->dir);
    CreateDirectoryA(p->games, NULL);
    snprintf(p->core, sizeof p->core, "%s\\dosbox_pure_libretro.dll", p->dir);
    GetTempPathA(sizeof p->temp, p->temp);
    snprintf(logpath, sizeof logpath, "%s\\%s", p->dir, log_name);

    InitializeCriticalSection(&g_loglock);
    g_logfile = fopen(logpath, "w");
    applog_set_sink(log_sink);
    if (getenv("VFX1_LOG_URL")) applog_init(getenv("VFX1_LOG_URL"));
    SetUnhandledExceptionFilter(on_crash);
    timeBeginPeriod(1);
    applog(APPLOG_INFO, TAG, "starting: games in %s", p->games);
}

const char *winplat_server(const char *builtin)
{
    return getenv("VFX1_SERVER") ? getenv("VFX1_SERVER") : builtin;
}

double winplat_now(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

void winplat_stop(void)
{
    if (g_logfile) fclose(g_logfile);
    g_logfile = NULL;
}

/* ------------------------------------------------------------ audio */

#define AUDIO_BUFFERS 4
#define AUDIO_FRAMES 1024               /* about 21 ms each at 48 kHz */

static HWAVEOUT g_wave;
static HANDLE g_wave_event, g_audio_thread;
static volatile LONG g_audio_quit;

static DWORD WINAPI audio_thread(void *arg)
{
    VfxApp *vfx = (VfxApp *)arg;
    static int16_t data[AUDIO_BUFFERS][AUDIO_FRAMES * 2];
    WAVEHDR hdr[AUDIO_BUFFERS];
    int i;
    memset(hdr, 0, sizeof hdr);
    for (i = 0; i < AUDIO_BUFFERS; i++) {
        hdr[i].lpData = (LPSTR)data[i];
        hdr[i].dwBufferLength = sizeof data[i];
        waveOutPrepareHeader(g_wave, &hdr[i], sizeof hdr[i]);
        memset(data[i], 0, sizeof data[i]);
        waveOutWrite(g_wave, &hdr[i], sizeof hdr[i]);
    }
    while (!g_audio_quit) {
        WaitForSingleObject(g_wave_event, 100);
        for (i = 0; i < AUDIO_BUFFERS; i++)
            if (hdr[i].dwFlags & WHDR_DONE) {
                vfxapp_audio_pull(vfx, data[i], AUDIO_FRAMES);
                waveOutWrite(g_wave, &hdr[i], sizeof hdr[i]);
            }
    }
    waveOutReset(g_wave);
    for (i = 0; i < AUDIO_BUFFERS; i++) waveOutUnprepareHeader(g_wave, &hdr[i], sizeof hdr[i]);
    return 0;
}

void winplat_audio_start(VfxApp *vfx, int rate)
{
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = (DWORD)rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = (DWORD)rate * 4;
    g_wave_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &fmt, (DWORD_PTR)g_wave_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        applog(APPLOG_ERROR, TAG, "no sound output");
        return;
    }
    g_audio_thread = CreateThread(NULL, 0, audio_thread, vfx, 0, NULL);
}

void winplat_audio_stop(void)
{
    if (!g_audio_thread) return;
    InterlockedExchange(&g_audio_quit, 1);
    WaitForSingleObject(g_audio_thread, 1000);
    waveOutClose(g_wave);
    g_audio_thread = NULL;
}

/* ------------------------------------------------------------ keyboard */

unsigned winplat_retrok_from_vk(WPARAM vk, LPARAM lp)
{
    int ext = (int)((lp >> 24) & 1), scan = (int)((lp >> 16) & 0xFF);
    if (vk >= 'A' && vk <= 'Z') return RETROK_a + (unsigned)(vk - 'A');
    if (vk >= '0' && vk <= '9') return RETROK_0 + (unsigned)(vk - '0');
    if (vk >= VK_F1 && vk <= VK_F10) return RETROK_F1 + (unsigned)(vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return RETROK_KP0 + (unsigned)(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_RETURN: return ext ? RETROK_KP_ENTER : RETROK_RETURN;
    case VK_ESCAPE: return RETROK_ESCAPE;
    case VK_SPACE: return RETROK_SPACE;
    case VK_TAB: return RETROK_TAB;
    case VK_BACK: return RETROK_BACKSPACE;
    case VK_SHIFT: return scan == 0x36 ? RETROK_RSHIFT : RETROK_LSHIFT;
    case VK_CONTROL: return ext ? RETROK_RCTRL : RETROK_LCTRL;
    case VK_MENU: return ext ? RETROK_RALT : RETROK_LALT;
    case VK_UP: return RETROK_UP;
    case VK_DOWN: return RETROK_DOWN;
    case VK_LEFT: return RETROK_LEFT;
    case VK_RIGHT: return RETROK_RIGHT;
    case VK_INSERT: return RETROK_INSERT;
    case VK_DELETE: return RETROK_DELETE;
    case VK_HOME: return RETROK_HOME;
    case VK_END: return RETROK_END;
    case VK_PRIOR: return RETROK_PAGEUP;
    case VK_NEXT: return RETROK_PAGEDOWN;
    case VK_DECIMAL: return RETROK_KP_PERIOD;
    case VK_ADD: return RETROK_KP_PLUS;
    case VK_SUBTRACT: return RETROK_KP_MINUS;
    case VK_MULTIPLY: return RETROK_KP_MULTIPLY;
    case VK_DIVIDE: return RETROK_KP_DIVIDE;
    case VK_OEM_MINUS: return RETROK_MINUS;
    case VK_OEM_PLUS: return RETROK_EQUALS;
    case VK_OEM_4: return RETROK_LEFTBRACKET;
    case VK_OEM_6: return RETROK_RIGHTBRACKET;
    case VK_OEM_1: return RETROK_SEMICOLON;
    case VK_OEM_7: return RETROK_QUOTE;
    case VK_OEM_COMMA: return RETROK_COMMA;
    case VK_OEM_PERIOD: return RETROK_PERIOD;
    case VK_OEM_2: return RETROK_SLASH;
    case VK_OEM_5: return RETROK_BACKSLASH;
    case VK_OEM_3: return RETROK_BACKQUOTE;
    case VK_CAPITAL: return RETROK_CAPSLOCK;
    case VK_NUMLOCK: return RETROK_NUMLOCK;
    case VK_SCROLL: return RETROK_SCROLLOCK;
    case VK_PAUSE: return RETROK_PAUSE;
    default: return RETROK_UNKNOWN;
    }
}
