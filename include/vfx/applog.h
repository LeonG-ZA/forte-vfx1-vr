/*
 * applog.h -- the apps' log: each platform's own output (logcat, a console
 * and file) and, once applog_init has a URL, the dev PC's web/serve.py
 * (POST /applog), for a headset without adb.
 */
#ifndef VFX_APPLOG_H
#define VFX_APPLOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Priorities, Android's numbers. */
enum { APPLOG_INFO = 4, APPLOG_WARN = 5, APPLOG_ERROR = 6, APPLOG_FATAL = 7 };

/* Where lines go locally; the platform sets it once at start. */
typedef void (*ApplogSink)(int prio, const char *tag, const char *line);
void applog_set_sink(ApplogSink sink);

void applog_init(const char *url);
void applog(int prio, const char *tag, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
/* Send one line to the dev PC now, without the queue: for crash handlers. */
void applog_post_now(const char *text);

#ifdef __cplusplus
}
#endif

#endif
