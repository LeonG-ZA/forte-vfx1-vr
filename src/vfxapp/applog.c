/*
 * applog.c -- see include/vfx/applog.h.
 *
 * Lines for the dev PC wait in a queue for a sender thread, so a slow or
 * absent network never holds up the caller; when the queue is full, new
 * lines are dropped.
 */
#define _GNU_SOURCE
#include "vfx/applog.h"
#include "net.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char g_host[256], g_port[8] = "80", g_path[256] = "/applog";
static int g_enabled;
static ApplogSink g_sink;

#define QUEUE_LINES 256
static char g_queue[QUEUE_LINES][1100];
static int g_head, g_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;

void applog_set_sink(ApplogSink sink)
{
    g_sink = sink;
}

static void *sender(void *arg)
{
    char msg[1100];
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (g_count == 0) pthread_cond_wait(&g_wake, &g_lock);
        memcpy(msg, g_queue[g_head], sizeof msg);
        g_head = (g_head + 1) % QUEUE_LINES;
        g_count--;
        pthread_mutex_unlock(&g_lock);
        applog_post_now(msg);
    }
    return NULL;
}

static void enqueue(const char *msg)
{
    pthread_mutex_lock(&g_lock);
    if (g_count < QUEUE_LINES) {                       /* full: the line is dropped */
        snprintf(g_queue[(g_head + g_count) % QUEUE_LINES], sizeof g_queue[0], "%s", msg);
        g_count++;
        pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_lock);
}

void applog_init(const char *url)
{
    const char *p, *slash, *colon;
    if (!url || strncmp(url, "http://", 7)) return;
    p = url + 7;
    slash = strchr(p, '/');
    colon = memchr(p, ':', slash ? (size_t)(slash - p) : strlen(p));
    if (colon) {
        snprintf(g_host, sizeof g_host, "%.*s", (int)(colon - p), p);
        snprintf(g_port, sizeof g_port, "%.*s", (int)((slash ? slash : p + strlen(p)) - colon - 1), colon + 1);
    } else {
        snprintf(g_host, sizeof g_host, "%.*s", (int)(slash ? (size_t)(slash - p) : strlen(p)), p);
    }
    if (slash) snprintf(g_path, sizeof g_path, "%s", slash);
    net_start();
    {
        pthread_t t;
        if (pthread_create(&t, NULL, sender, NULL) != 0) return;
        pthread_detach(t);
    }
    g_enabled = 1;
}

void applog_post_now(const char *text)
{
    struct addrinfo hints, *res = NULL;
    char head[512], reply[64];
    net_socket fd;
    size_t len = strlen(text);

    if (!g_enabled) return;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_host, g_port, &hints, &res) != 0 || !res) return;
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd != NET_BAD) {
        net_timeouts(fd, 1);
        if (connect(fd, res->ai_addr, (int)res->ai_addrlen) == 0) {
            int n = snprintf(head, sizeof head,
                             "POST %s HTTP/1.0\r\nHost: %s\r\nContent-Type: text/plain\r\nContent-Length: %lu\r\n\r\n",
                             g_path, g_host, (unsigned long)len);
            if (send(fd, head, n, 0) == n && send(fd, text, (int)len, 0) == (net_ssize)len)
                recv(fd, reply, (int)sizeof reply, 0);
        }
        net_close(fd);
    }
    freeaddrinfo(res);
}

void applog(int prio, const char *tag, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_sink) g_sink(prio, tag, line);
    else fprintf(stderr, "%s: %s\n", tag, line);
    if (g_enabled) {
        char msg[1100];
        snprintf(msg, sizeof msg, "%s %s: %s", prio >= APPLOG_ERROR ? "E" : prio == APPLOG_WARN ? "W" : "I", tag, line);
        enqueue(msg);
    }
}
