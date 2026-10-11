/*
 * fetch.c -- download one file over plain HTTP, for getting a game onto the
 * headset without adb: the dev PC's web/serve.py serves it on the LAN.
 */
#define _POSIX_C_SOURCE 200809L
#include "fetch.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_temp_dir[512];

void fetch_set_temp_dir(const char *dir)
{
    snprintf(g_temp_dir, sizeof g_temp_dir, "%s", dir ? dir : "");
}

int http_download(const char *url, const char *dest, char *err, size_t err_size)
{
    char host[256], port[8] = "80", tmp[1024];
    const char *p, *slash, *colon, *path;
    struct addrinfo hints, *res = NULL, *ai;
    net_socket fd = NET_BAD;
    int status = 0;
    FILE *out = NULL;
    char buf[65536], req[1400];
    size_t have = 0;
    long total = 0;
    char *body;

    if (strncmp(url, "http://", 7)) { snprintf(err, err_size, "only http:// URLs"); return 0; }
    p = url + 7;
    slash = strchr(p, '/');
    path = slash ? slash : "/";
    colon = memchr(p, ':', slash ? (size_t)(slash - p) : strlen(p));
    if (colon) {
        snprintf(host, sizeof host, "%.*s", (int)(colon - p), p);
        snprintf(port, sizeof port, "%.*s", (int)((slash ? slash : p + strlen(p)) - colon - 1), colon + 1);
    } else {
        snprintf(host, sizeof host, "%.*s", (int)(slash ? (size_t)(slash - p) : strlen(p)), p);
    }

    net_start();
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) { snprintf(err, err_size, "cannot resolve %s", host); return 0; }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == NET_BAD) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        net_close(fd);
        fd = NET_BAD;
    }
    freeaddrinfo(res);
    if (fd == NET_BAD) { snprintf(err, err_size, "cannot connect to %s:%s", host, port); return 0; }

    snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (send(fd, req, (int)strlen(req), 0) < 0) { snprintf(err, err_size, "send failed"); net_close(fd); return 0; }

    /* headers */
    for (;;) {
        net_ssize n = recv(fd, buf + have, (int)(sizeof buf - 1 - have), 0);
        if (n <= 0) { snprintf(err, err_size, "connection closed in headers"); net_close(fd); return 0; }
        have += (size_t)n;
        buf[have] = 0;
        if ((body = strstr(buf, "\r\n\r\n"))) break;
        if (have == sizeof buf - 1) { snprintf(err, err_size, "headers too long"); net_close(fd); return 0; }
    }
    sscanf(buf, "HTTP/%*s %d", &status);
    if (status != 200) { snprintf(err, err_size, "HTTP %d for %s", status, url); net_close(fd); return 0; }
    body += 4;

    snprintf(tmp, sizeof tmp, "%s.part", dest);
    out = fopen(tmp, "wb");
    if (!out) { snprintf(err, err_size, "cannot write %s", tmp); net_close(fd); return 0; }
    fwrite(body, 1, have - (size_t)(body - buf), out);
    total = (long)(have - (size_t)(body - buf));
    for (;;) {
        net_ssize n = recv(fd, buf, (int)sizeof buf, 0);
        if (n < 0) { snprintf(err, err_size, "receive failed"); fclose(out); net_close(fd); remove(tmp); return 0; }
        if (n == 0) break;
        fwrite(buf, 1, (size_t)n, out);
        total += n;
    }
    net_close(fd);
    remove(dest);                                   /* Windows will not rename over a file */
    if (fclose(out) != 0 || rename(tmp, dest) != 0) { snprintf(err, err_size, "cannot finish %s", dest); remove(tmp); return 0; }
    snprintf(err, err_size, "%ld bytes", total);
    return 1;
}

int http_get_text(const char *url, char *out, size_t size, char *err, size_t err_size)
{
    char tmp[600];
    FILE *f;
    size_t n;
    /* Reuse the download path into a private temp file. */
    snprintf(tmp, sizeof tmp, "%s/listing.html", g_temp_dir[0] ? g_temp_dir : ".");
    if (!http_download(url, tmp, err, err_size)) return 0;
    f = fopen(tmp, "rb");
    if (!f) { snprintf(err, err_size, "cannot read listing"); return 0; }
    n = fread(out, 1, size - 1, f);
    out[n] = 0;
    fclose(f);
    remove(tmp);
    return 1;
}
