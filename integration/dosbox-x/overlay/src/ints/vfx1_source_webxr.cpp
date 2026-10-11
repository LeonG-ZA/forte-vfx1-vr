/*
 *  VFX1 head tracking: the WebXR pose source. GPL v2 or later, as DOSBox-X.
 *
 *  A WebXR page cannot open a raw socket and an emulator cannot call WebXR, so
 *  the two meet over a WebSocket on the loopback interface. This file is the
 *  emulator end: a tiny single-client HTTP and WebSocket server that serves the
 *  page and then receives head poses from it.
 *
 *  Why this shape rather than an in-process XR runtime: the browser already
 *  has a working, permission-gated, vendor-neutral path to every headset the
 *  user owns, and it is the XR API a browser offers. It
 *  also decouples the machine running the emulator from the machine wearing
 *  the headset, which matters when the emulator is in a VM or a container.
 *
 *  This file is a pose source, so it is allowed to name exactly one transport.
 *  vfx1_mouse.cpp, vfx1.h and libvfxbridge name none, and the layering gate
 *  checks that. The protocol itself is in src/wsproto, which has no I/O in it
 *  and is unit tested against the RFC 6455 vectors.
 *
 *  Security: the listener binds to 127.0.0.1 only and accepts one client at a
 *  time. It is not a general-purpose web server and must never be exposed.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "dosbox.h"
#include "logging.h"
#include "pic.h"
#include "setup.h"
#include "control.h"
#include "vfx1.h"

extern "C" {
#include "wsproto.h"
}

#if defined(_WIN32) || defined(WIN32)
# include <winsock2.h>
# include <ws2tcpip.h>
  typedef SOCKET vfx_socket_t;
# define VFX_INVALID_SOCKET INVALID_SOCKET
# define vfx_closesocket closesocket
# define VFX_WOULDBLOCK   (WSAGetLastError() == WSAEWOULDBLOCK)
#else
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <arpa/inet.h>
# include <unistd.h>
# include <fcntl.h>
# include <errno.h>
  typedef int vfx_socket_t;
# define VFX_INVALID_SOCKET (-1)
# define vfx_closesocket close
# define VFX_WOULDBLOCK   (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

/* One pose message is tens of bytes. This is generous and bounded. */
#define VFX_RX_CAP   16384u
#define VFX_PAGE_CAP (512u * 1024u)

static vfx_socket_t net_listen_fd = VFX_INVALID_SOCKET;
static vfx_socket_t net_client_fd = VFX_INVALID_SOCKET;

static uint8_t  net_rx[VFX_RX_CAP];
static size_t   net_rx_len   = 0;
static bool     net_upgraded = false;

static char    *net_page      = NULL;   /* the WebXR page, read once at open */
static size_t   net_page_len  = 0;

static VfxPose  net_pose;
static bool     net_have_pose = false;
static uint64_t net_messages  = 0;
static double   net_last_msg_ms = 0.0;

/* Read from config at open time. */
static int      net_port = 8085;
static char     net_page_path[512] = "";

/* ------------------------------------------------------------------ */
/* socket helpers                                                      */
/* ------------------------------------------------------------------ */

static void set_nonblocking(vfx_socket_t s)
{
#if defined(_WIN32) || defined(WIN32)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0)
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

static void drop_client(const char *why)
{
    if (net_client_fd != VFX_INVALID_SOCKET) {
        vfx_closesocket(net_client_fd);
        net_client_fd = VFX_INVALID_SOCKET;
        if (why != NULL)
            LOG(LOG_MOUSE, LOG_DEBUG)("VFX1: webxr client dropped (%s)", why);
    }
    net_rx_len = 0;
    net_upgraded = false;
}

static bool send_all(const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t sent = 0;
    int spins = 0;

    while (sent < len) {
        int n = (int)send(net_client_fd, p + sent, (int)(len - sent), 0);
        if (n > 0) {
            sent += (size_t)n;
            spins = 0;
        } else if (n < 0 && VFX_WOULDBLOCK) {
            /* The socket is non-blocking and these writes are small, so a
             * would-block here means the peer is wedged. Give up rather than
             * stall the emulator's tick handler. */
            if (++spins > 1000)
                return false;
        } else {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

static void serve_page(void)
{
    char hdr[256];
    int n;

    if (net_page == NULL || net_page_len == 0) {
        static const char nf[] =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send_all(nf, sizeof(nf) - 1u);
        return;
    }

    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n\r\n",
                 (unsigned long)net_page_len);
    if (n > 0 && send_all(hdr, (size_t)n))
        send_all(net_page, net_page_len);
}

/* Returns true if the connection should stay open. */
static bool handle_http(void)
{
    char key[128], upgrade[64], accept[VFX_WS_ACCEPT_LEN], target[128];
    char resp[512];
    int n;

    /* The request is text; NUL-terminate a copy for the parsers. */
    if (net_rx_len >= VFX_RX_CAP)
        return false;
    net_rx[net_rx_len] = 0;

    if (!vfx_http_request_target((const char *)net_rx, "GET", target, sizeof(target)))
        return false;

    if (!vfx_http_header((const char *)net_rx, "Upgrade", upgrade, sizeof(upgrade)) ||
        strncmp(upgrade, "websocket", 9) != 0) {
        /* An ordinary page request. Serve it and close, as advertised. */
        serve_page();
        return false;
    }

    if (!vfx_http_header((const char *)net_rx, "Sec-WebSocket-Key", key, sizeof(key)))
        return false;
    if (!vfx_ws_accept_key(key, accept, sizeof(accept)))
        return false;

    n = snprintf(resp, sizeof(resp),
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n\r\n",
                 accept);
    if (n <= 0 || !send_all(resp, (size_t)n))
        return false;

    net_upgraded = true;
    net_rx_len = 0;
    LOG_MSG("VFX1: webxr page connected");
    return true;
}

/* ------------------------------------------------------------------ */
/* WebSocket                                                           */
/* ------------------------------------------------------------------ */

static void apply_message(const char *text, size_t len)
{
    VfxWireMsg m;

    if (!vfx_wire_parse(text, len, &m))
        return;

    if (m.kind == VFX_WIRE_RECENTER) {
        VFX1_Mouse_Recenter();
        return;
    }
    if (m.kind != VFX_WIRE_POSE)
        return;

    memset(&net_pose, 0, sizeof(net_pose));
    net_pose.orientation.w = m.w;
    net_pose.orientation.x = m.x;
    net_pose.orientation.y = m.y;
    net_pose.orientation.z = m.z;
    net_pose.timestamp_ns  = (uint64_t)(PIC_FullIndex() * 1.0e6);

    net_have_pose = true;
    ++net_messages;
    net_last_msg_ms = PIC_FullIndex();
}

/* Returns true if the connection should stay open. */
static bool pump_frames(void)
{
    for (;;) {
        VfxWsFrame fr;
        int r = vfx_ws_decode(net_rx, net_rx_len, &fr);

        if (r == 0)
            return true;                     /* need more bytes */
        if (r < 0)
            return false;                    /* protocol error */

        switch (fr.opcode) {
        case VFX_WS_TEXT:
            apply_message((const char *)fr.payload, fr.payload_len);
            break;
        case VFX_WS_PING: {
            uint8_t out[VFX_WS_MAX_PAYLOAD / 8u];
            size_t n = vfx_ws_encode(out, sizeof(out), VFX_WS_PONG,
                                     fr.payload, fr.payload_len);
            if (n == 0 || !send_all(out, n))
                return false;
            break;
        }
        case VFX_WS_CLOSE:
            return false;
        default:
            break;                           /* ignore anything else */
        }

        memmove(net_rx, net_rx + fr.consumed, net_rx_len - fr.consumed);
        net_rx_len -= fr.consumed;
    }
}

/* ------------------------------------------------------------------ */
/* VfxPoseSource                                                       */
/* ------------------------------------------------------------------ */

static void load_page(void)
{
    FILE *f;
    long sz;

    free(net_page);
    net_page = NULL;
    net_page_len = 0;

    if (net_page_path[0] == '\0')
        return;

    f = fopen(net_page_path, "rb");
    if (f == NULL) {
        LOG_MSG("VFX1: cannot open webxr page '%s'; the server will still "
                "accept a connection, but will not serve the page",
                net_page_path);
        return;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz > 0 && (size_t)sz <= VFX_PAGE_CAP) {
        net_page = (char *)malloc((size_t)sz);
        if (net_page != NULL && fread(net_page, 1, (size_t)sz, f) == (size_t)sz)
            net_page_len = (size_t)sz;
        else {
            free(net_page);
            net_page = NULL;
        }
    } else {
        LOG_MSG("VFX1: webxr page '%s' is empty or too large", net_page_path);
    }
    fclose(f);
}

static bool src_net_open(void)
{
    Section_prop *sec = static_cast<Section_prop *>(control->GetSection("vfx1"));
    struct sockaddr_in addr;
    int one = 1;

    if (sec != NULL) {
        net_port = sec->Get_int("webxr port");
        const char *p = sec->Get_string("webxr page");
        if (p != NULL) {
            strncpy(net_page_path, p, sizeof(net_page_path) - 1u);
            net_page_path[sizeof(net_page_path) - 1u] = '\0';
        }
    }
    if (net_port < 1 || net_port > 65535) {
        LOG_MSG("VFX1: webxr port %d out of range", net_port);
        return false;
    }

#if defined(_WIN32) || defined(WIN32)
    {
        WSADATA wsa;
        static bool wsa_started = false;
        if (!wsa_started) {
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                LOG_MSG("VFX1: WSAStartup failed");
                return false;
            }
            wsa_started = true;
        }
    }
#endif

    net_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (net_listen_fd == VFX_INVALID_SOCKET) {
        LOG_MSG("VFX1: cannot create webxr socket");
        return false;
    }
    setsockopt(net_listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)net_port);
    /* Loopback only. This is not a server anyone else should reach. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(net_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_MSG("VFX1: cannot bind 127.0.0.1:%d for webxr (already in use?)",
                net_port);
        vfx_closesocket(net_listen_fd);
        net_listen_fd = VFX_INVALID_SOCKET;
        return false;
    }
    if (listen(net_listen_fd, 1) != 0) {
        LOG_MSG("VFX1: cannot listen on 127.0.0.1:%d", net_port);
        vfx_closesocket(net_listen_fd);
        net_listen_fd = VFX_INVALID_SOCKET;
        return false;
    }
    set_nonblocking(net_listen_fd);

    load_page();
    net_have_pose = false;
    net_messages = 0;

    LOG_MSG("VFX1: webxr pose server on http://localhost:%d/ (page: %s)",
            net_port, net_page_len ? net_page_path : "none");
    return true;
}

static void src_net_close(void)
{
    drop_client(NULL);
    if (net_listen_fd != VFX_INVALID_SOCKET) {
        vfx_closesocket(net_listen_fd);
        net_listen_fd = VFX_INVALID_SOCKET;
    }
    free(net_page);
    net_page = NULL;
    net_page_len = 0;
}

static bool src_net_poll(VfxPose *out)
{
    /* Accept a client if we do not have one. One at a time: a second headset
     * would fight the first for the same mouse. */
    if (net_client_fd == VFX_INVALID_SOCKET &&
        net_listen_fd != VFX_INVALID_SOCKET) {
        vfx_socket_t c = accept(net_listen_fd, NULL, NULL);
        if (c != VFX_INVALID_SOCKET) {
            int one = 1;
            set_nonblocking(c);
            /* Poses are tiny and latency-critical, so do not let Nagle sit on
             * them waiting for a bigger packet. */
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                       (const char *)&one, sizeof(one));
            net_client_fd = c;
            net_rx_len = 0;
            net_upgraded = false;
        }
    }

    while (net_client_fd != VFX_INVALID_SOCKET) {
        int n;

        if (net_rx_len + 1u >= VFX_RX_CAP) {
            /* A well-behaved page never gets close to this. */
            drop_client("receive buffer full");
            break;
        }

        n = (int)recv(net_client_fd, (char *)net_rx + net_rx_len,
                      (int)(VFX_RX_CAP - net_rx_len - 1u), 0);
        if (n > 0) {
            net_rx_len += (size_t)n;
        } else if (n == 0) {
            drop_client("closed by peer");
            break;
        } else if (VFX_WOULDBLOCK) {
            break;                            /* nothing more right now */
        } else {
            drop_client("receive error");
            break;
        }

        if (!net_upgraded) {
            if (!vfx_http_headers_complete((const char *)net_rx, net_rx_len))
                continue;                     /* wait for the rest */
            if (!handle_http()) {
                drop_client(NULL);
                break;
            }
        }
        if (net_upgraded && !pump_frames()) {
            drop_client("websocket closed");
            break;
        }
    }

    if (!net_have_pose)
        return false;

    /* Hand back the most recent pose even when nothing new arrived: a steady
     * pose produces a zero delta, which is exactly right. */
    *out = net_pose;
    return true;
}

const VfxPoseSource *VFX1_Source_Net(void)
{
    static const VfxPoseSource s = {
        "webxr", src_net_open, src_net_close, src_net_poll
    };
    return &s;
}
