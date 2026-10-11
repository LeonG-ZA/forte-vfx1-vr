/*
 * retrohost.h -- a minimal libretro frontend for one DOS core.
 *
 * Loads a libretro core (DOSBox Pure with the VFX1, integration/dosbox-pure),
 * answers the environment calls a DOS core needs, runs it a frame at a time,
 * and hands back each frame as RGB24, which is what vfxhost's page-flip
 * extractor takes. Keyboard and mouse state go in; audio comes out through a
 * callback. If the core exports vfx1Pose/vfx1Stats, the VFX1 is reachable
 * too.
 *
 * Shared by the headless test (src/adapters/libretro-headless) and the native
 * OpenXR app. libretro's callbacks are global, so only one host can be open
 * at a time. Not thread safe.
 */
#ifndef VFX_RETROHOST_H
#define VFX_RETROHOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RetroHost RetroHost;

/* Audio frames, interleaved stereo int16, at retrohost_sample_rate(). */
typedef void (*retrohost_audio_fn)(void *user, const int16_t *frames, size_t count);
/* Core log lines, for the host's own log. */
typedef void (*retrohost_log_fn)(void *user, int level, const char *line);

RetroHost *retrohost_open(const char *core_path, const char *system_dir, const char *save_dir,
                          char *err, size_t err_size);
void retrohost_close(RetroHost *h);

void retrohost_set_log(RetroHost *h, retrohost_log_fn fn, void *user);
void retrohost_set_audio(RetroHost *h, retrohost_audio_fn fn, void *user);
/* Core option, e.g. "dosbox_pure_cycles" = "max". Before or after loading. */
int retrohost_set_option(RetroHost *h, const char *key, const char *value);

int retrohost_load(RetroHost *h, const char *content_path);
/* Unload the running content, so another can be loaded into the same core. */
void retrohost_unload(RetroHost *h);
int retrohost_loaded(const RetroHost *h);

/* The core's options, as it declared them (libretro SET_VARIABLES): a key, a
 * label and a list of allowed values, the first being the default. */
int retrohost_option_count(const RetroHost *h);
const char *retrohost_option_key(const RetroHost *h, int index);
const char *retrohost_option_label(const RetroHost *h, int index);
int retrohost_option_value_count(const RetroHost *h, int index);
const char *retrohost_option_value(const RetroHost *h, int index, int value);
/* The value in effect: set by the host, or the core's default. */
const char *retrohost_option_current(const RetroHost *h, const char *key);
void retrohost_run(RetroHost *h);                       /* one retro_run */
double retrohost_fps(const RetroHost *h);
double retrohost_sample_rate(const RetroHost *h);

/* The newest frame as RGB24; serial rises with every new frame. */
const uint8_t *retrohost_frame(const RetroHost *h, int *width, int *height, uint64_t *serial);

/* Keyboard by libretro key code (RETROK_*), mouse as relative motion. */
void retrohost_key(RetroHost *h, unsigned retrok, int down);
void retrohost_mouse(RetroHost *h, int dx, int dy, unsigned buttons);

/* VFX1: 1 if the core has the device. */
int retrohost_has_vfx1(const RetroHost *h);
void retrohost_vfx1_pose(RetroHost *h, int yaw, int pitch, int roll,
                         int puck_pitch, int puck_roll, int buttons);
/* Gain on the head angles the i-glasses and CyberMaxx trackers report. */
void retrohost_vfx1_tracker_gain(RetroHost *h, double gain);
/* Plugs an emulated head tracker into COM `port` (1-4) for the next content
 * loaded, overriding its dosbox.conf: kind -1 none, 0 i-glasses, 1 CyberMaxx. */
void retrohost_vfx1_serial_tracker(RetroHost *h, int port, int kind);
/* 1: the title steers by head movement (Descent); match its frame rate. */
void retrohost_vfx1_steer(RetroHost *h, int on);
/* poses, psrWrites, psr, statusCalls, dataCalls, reportCalls, zeroCalls,
 * videoMode, vipWrites; NULL without the device. */
const uint32_t *retrohost_vfx1_stats(RetroHost *h);

#ifdef __cplusplus
}
#endif

#endif /* VFX_RETROHOST_H */
