/*
 * net.h -- the few socket calls fetch.c and applog.c use, on POSIX and on
 * Windows (Winsock).
 */
#ifndef VFXAPP_NET_H
#define VFXAPP_NET_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET net_socket;
#define NET_BAD INVALID_SOCKET
#define net_close closesocket
typedef int net_ssize;
/* Winsock wants starting once per process; harmless to repeat. */
static inline void net_start(void)
{
    static int started;
    WSADATA wsa;
    if (!started && WSAStartup(MAKEWORD(2, 2), &wsa) == 0) started = 1;
}
static inline void net_timeouts(net_socket fd, int seconds)
{
    DWORD ms = (DWORD)seconds * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
}
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int net_socket;
#define NET_BAD (-1)
#define net_close close
typedef ssize_t net_ssize;
static inline void net_start(void) {}
static inline void net_timeouts(net_socket fd, int seconds)
{
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
#endif

#endif
