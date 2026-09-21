/*
 * BGB-compatible Game Boy link cable over TCP.
 *
 * Protocol reference: https://bgb.bircd.org/bgblink.html
 * Each packet is exactly 8 bytes, little-endian:
 *   b1 cmd, b2 b3 b4 data, i1 (uint32) timestamp.
 *
 * Threading: a single background thread does blocking recv() and pushes parsed
 * packets onto a mutex-guarded ring. The platform main thread is the only
 * sender (drains the inbound ring in gb_serial_link_tick and writes responses
 * inline) — keeps the wire-write path single-threaded so 8-byte packets never
 * interleave.
 */

#include "serial_link.h"
#include "gbrt.h"

#include "gb_socket.h"
#include <atomic>
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system_error>
#include <thread>

#define BGB_CMD_VERSION 1
#define BGB_CMD_JOYPAD 101
#define BGB_CMD_SYNC1 104 /* master sent a byte */
#define BGB_CMD_SYNC2 105 /* slave's reply byte */
#define BGB_CMD_SYNC3 106 /* sync without data (timestamp/keepalive) */
#define BGB_CMD_STATUS 108
#define BGB_CMD_DISCONNECT 109

#define BGB_PROTO_MAJOR 1
#define BGB_PROTO_MINOR 4

#define INBOX_CAP 64

typedef struct {
    uint8_t cmd;
    uint8_t b2;
    uint8_t b3;
    uint8_t b4;
    uint32_t timestamp;
} BGBPacket;

typedef enum {
    LINK_OFF = 0,
    LINK_LISTEN,
    LINK_CONNECT,
} LinkMode;

static struct LinkState {
    LinkMode mode = LINK_OFF;
    gb_net::Environment network;
    gb_net::Socket listen_fd = gb_net::invalid;
    std::atomic<gb_net::Socket> peer_fd{gb_net::invalid};
    char peer_ip[64]{};
    // Publishing connected also publishes peer_ip to the main thread.
    std::atomic<bool> connected{false};
    std::atomic<bool> version_ok{false};
    std::atomic<bool> peer_ready{false};
    std::atomic<bool> shutdown_requested{false};
    std::thread thread;
    std::mutex inbox_mutex;
    BGBPacket inbox[INBOX_CAP]{};
    int inbox_head = 0;
    int inbox_tail = 0;
    // Touched only by the main thread.
    bool master_pending = false;
    bool initial_handshake_done = false;
    ~LinkState() { gb_serial_link_shutdown(); }
} g;

static void link_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[LINK] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void inbox_push_locked(BGBPacket pkt) {
    int next = (g.inbox_tail + 1) % INBOX_CAP;
    if (next == g.inbox_head) {
        /* Drop oldest to make room — better than blocking the net thread. */
        g.inbox_head = (g.inbox_head + 1) % INBOX_CAP;
    }
    g.inbox[g.inbox_tail] = pkt;
    g.inbox_tail = next;
}

static bool inbox_pop(BGBPacket *out) {
    std::lock_guard<std::mutex> lock(g.inbox_mutex);
    bool ok = false;
    if (g.inbox_head != g.inbox_tail) {
        *out = g.inbox[g.inbox_head];
        g.inbox_head = (g.inbox_head + 1) % INBOX_CAP;
        ok = true;
    }
    return ok;
}

static bool send_all(gb_net::Socket fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        int n = gb_net::send(fd, p + sent, len - sent);
        if (n <= 0) {
            if (n < 0 && gb_net::interrupted())
                continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool recv_all(gb_net::Socket fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        if (g.shutdown_requested)
            return false;
        int ready = gb_net::readable(fd, 250);
        if (ready == 0)
            continue;
        if (ready < 0) {
            if (gb_net::interrupted())
                continue;
            return false;
        }
        int n = gb_net::receive(fd, p + got, len - got);
        if (n == 0)
            return false; /* peer closed */
        if (n < 0) {
            if (gb_net::interrupted())
                continue;
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static bool send_packet(BGBPacket pkt) {
    if (g.peer_fd == gb_net::invalid)
        return false;
    uint8_t buf[8];
    buf[0] = pkt.cmd;
    buf[1] = pkt.b2;
    buf[2] = pkt.b3;
    buf[3] = pkt.b4;
    buf[4] = (uint8_t)(pkt.timestamp & 0xFF);
    buf[5] = (uint8_t)((pkt.timestamp >> 8) & 0xFF);
    buf[6] = (uint8_t)((pkt.timestamp >> 16) & 0xFF);
    buf[7] = (uint8_t)((pkt.timestamp >> 24) & 0xFF);
    return send_all(g.peer_fd, buf, 8);
}

static bool recv_packet(gb_net::Socket fd, BGBPacket *out) {
    uint8_t buf[8];
    if (!recv_all(fd, buf, 8))
        return false;
    out->cmd = buf[0];
    out->b2 = buf[1];
    out->b3 = buf[2];
    out->b4 = buf[3];
    out->timestamp = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) |
                     ((uint32_t)buf[7] << 24);
    return true;
}

static void net_thread_main() {
    if (g.mode == LINK_LISTEN) {
        while (!g.shutdown_requested) {
            int ready = gb_net::readable(g.listen_fd, 250);
            if (ready == 0)
                continue;
            if (ready < 0) {
                if (gb_net::interrupted())
                    continue;
                link_log("listen wait failed: %s", gb_net::error().c_str());
                return;
            }
            sockaddr_in client{};
            gb_net::AddressLength clen = sizeof(client);
            auto peer = accept(g.listen_fd, (sockaddr *)&client, &clen);
            if (peer == gb_net::invalid) {
                if (gb_net::interrupted())
                    continue;
                link_log("accept() failed: %s", gb_net::error().c_str());
                return;
            }
            g.peer_fd = peer;
            inet_ntop(AF_INET, &client.sin_addr, g.peer_ip, sizeof(g.peer_ip));
            link_log("Peer connected from %s:%d", g.peer_ip, (int)ntohs(client.sin_port));
            break;
        }
    }
    // The worker owns its accepted socket until shutdown joins it. Even a
    // shutdown racing accept cannot leave a blocking recv or reused handle.
    if (g.shutdown_requested)
        return;
    int yes = 1;
    gb_net::option(g.peer_fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
    gb_net::option(g.peer_fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    if (!gb_net::timeout(g.peer_fd, SO_SNDTIMEO, 250))
        return;
    g.connected = true;

    /* Receive loop. The main thread sends — we only consume. */
    while (!g.shutdown_requested) {
        BGBPacket pkt;
        if (!recv_packet(g.peer_fd, &pkt)) {
            if (!g.shutdown_requested) {
                link_log("Peer disconnected");
            }
            break;
        }

        if (pkt.cmd == BGB_CMD_VERSION) {
            if (pkt.b2 == BGB_PROTO_MAJOR && pkt.b3 == BGB_PROTO_MINOR) {
                g.version_ok = true;
                link_log("BGB protocol version %d.%d confirmed", (int)pkt.b2, (int)pkt.b3);
            } else {
                link_log("Unsupported peer version %d.%d (need %d.%d), disconnecting", (int)pkt.b2,
                         (int)pkt.b3, BGB_PROTO_MAJOR, BGB_PROTO_MINOR);
                break;
            }
            continue;
        }
        if (pkt.cmd == BGB_CMD_DISCONNECT) {
            link_log("Peer requested disconnect");
            break;
        }
        if (pkt.cmd == BGB_CMD_STATUS) {
            /* bit 0 = running. Treat anything with bit 0 set as ready. */
            g.peer_ready = (pkt.b2 & 0x01) != 0;
            /* Hand to main thread so it can echo a status back. */
            std::lock_guard<std::mutex> lock(g.inbox_mutex);
            inbox_push_locked(pkt);
            continue;
        }
        if (pkt.cmd == BGB_CMD_SYNC1 || pkt.cmd == BGB_CMD_SYNC2 || pkt.cmd == BGB_CMD_SYNC3) {
            std::lock_guard<std::mutex> lock(g.inbox_mutex);
            inbox_push_locked(pkt);
            continue;
        }
        /* Ignore JOYPAD (101) and any unknown commands. */
    }

    g.connected = false;
    return;
}

bool gb_serial_link_start_listen(uint16_t port) {
    if (g.mode != LINK_OFF)
        return false;

    if (!g.network.start())
        return false;
    g.shutdown_requested = false;

    auto fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == gb_net::invalid) {
        link_log("socket() failed: %s", gb_net::error().c_str());
        g.network.stop();
        return false;
    }
    int yes = 1;
#ifdef _WIN32
    gb_net::option(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &yes, sizeof(yes));
#else
    gb_net::option(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        link_log("bind(:%d) failed: %s", (int)port, gb_net::error().c_str());
        gb_net::close(fd);
        g.network.stop();
        return false;
    }
    if (listen(fd, 1) < 0) {
        link_log("listen() failed: %s", gb_net::error().c_str());
        gb_net::close(fd);
        g.network.stop();
        return false;
    }

    g.listen_fd = fd;
    g.mode = LINK_LISTEN;
    link_log("Listening on TCP :%d (BGB protocol)", (int)port);

    try {
        g.thread = std::thread(net_thread_main);
    } catch (const std::system_error &error) {
        link_log("thread start failed: %s", error.what());
        gb_serial_link_shutdown();
        return false;
    }
    return true;
}

bool gb_serial_link_start_connect(const char *host, uint16_t port) {
    if (g.mode != LINK_OFF)
        return false;

    if (!g.network.start())
        return false;
    g.shutdown_requested = false;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || !res) {
        link_log("getaddrinfo(%s:%d) failed: %s", host, (int)port, std::to_string(err).c_str());
        g.network.stop();
        return false;
    }

    auto fd = gb_net::invalid;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == gb_net::invalid)
            continue;
        if (connect(fd, ai->ai_addr, static_cast<gb_net::AddressLength>(ai->ai_addrlen)) == 0)
            break;
        gb_net::close(fd);
        fd = gb_net::invalid;
    }
    freeaddrinfo(res);

    if (fd == gb_net::invalid) {
        link_log("connect(%s:%d) failed: %s", host, (int)port, gb_net::error().c_str());
        g.network.stop();
        return false;
    }

    g.peer_fd = fd;
    g.mode = LINK_CONNECT;
    snprintf(g.peer_ip, sizeof(g.peer_ip), "%s", host);
    link_log("Connected to %s:%d (BGB protocol)", host, (int)port);

    try {
        g.thread = std::thread(net_thread_main);
    } catch (const std::system_error &error) {
        link_log("thread start failed: %s", error.what());
        gb_serial_link_shutdown();
        return false;
    }
    return true;
}

void gb_serial_link_shutdown(void) {
    if (g.mode == LINK_OFF)
        return;
    g.shutdown_requested = true;
    if (g.connected)
        send_packet(BGBPacket{BGB_CMD_DISCONNECT, 0, 0, 0, 0});
    if (g.thread.joinable())
        g.thread.join();
    gb_net::close(g.peer_fd.exchange(gb_net::invalid));
    gb_net::close(g.listen_fd);
    g.listen_fd = gb_net::invalid;
    g.network.stop();
    g.mode = LINK_OFF;
    g.connected = false;
    g.version_ok = false;
    g.peer_ready = false;
    g.master_pending = false;
    g.initial_handshake_done = false;
    g.inbox_head = g.inbox_tail = 0;
    g.peer_ip[0] = '\0';
}

const char *gb_serial_link_peer_ip(void) { return g.connected ? g.peer_ip : ""; }

void gb_serial_link_init_from_env(void) {
    const char *listen_env = getenv("GB_LINK_LISTEN");
    const char *connect_env = getenv("GB_LINK_CONNECT");

    if (listen_env && listen_env[0]) {
        unsigned port = (unsigned)strtoul(listen_env, NULL, 10);
        if (port == 0 || port > 65535) {
            link_log("GB_LINK_LISTEN: invalid port '%s'", listen_env);
            return;
        }
        gb_serial_link_start_listen((uint16_t)port);
        return;
    }

    if (connect_env && connect_env[0]) {
        /* Accept "host:port" or "host" + GB_LINK_PORT. */
        char buf[256];
        strncpy(buf, connect_env, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *colon = strrchr(buf, ':');
        const char *host = buf;
        unsigned port = 8765; /* BGB default */
        if (colon) {
            *colon = '\0';
            port = (unsigned)strtoul(colon + 1, NULL, 10);
        } else {
            const char *port_env = getenv("GB_LINK_PORT");
            if (port_env && port_env[0]) {
                port = (unsigned)strtoul(port_env, NULL, 10);
            }
        }
        if (port == 0 || port > 65535) {
            link_log("GB_LINK_CONNECT: invalid port (%u)", port);
            return;
        }
        gb_serial_link_start_connect(host, (uint16_t)port);
    }
}

bool gb_serial_link_is_active(void) { return g.mode != LINK_OFF; }

bool gb_serial_link_is_ready(void) { return g.mode != LINK_OFF && g.connected && g.version_ok; }

static uint32_t bgb_timestamp_for(struct GBContext *ctx) {
    /* BGB timestamps are in 2 MiHz cycles; the CPU runs at ~4 MiHz. */
    return ctx ? static_cast<uint32_t>(ctx->cycles >> 1) : 0;
}

static void send_initial_handshake(struct GBContext *ctx) {
    BGBPacket version = {
        BGB_CMD_VERSION, BGB_PROTO_MAJOR, BGB_PROTO_MINOR, 0, 0,
    };
    BGBPacket status = {
        BGB_CMD_STATUS,
        0x01, /* running, not paused, no reconnect */
        0,
        0,
        bgb_timestamp_for(ctx),
    };
    send_packet(version);
    send_packet(status);
    g.initial_handshake_done = true;
}

void gb_serial_link_on_serial_byte(struct GBContext *ctx, uint8_t outgoing) {
    if (!gb_serial_link_is_ready()) {
        return;
    }
    BGBPacket sync1 = {
        BGB_CMD_SYNC1,
        outgoing,
        0x81, /* control bits: high, no double-speed */
        0,
        bgb_timestamp_for(ctx),
    };
    if (send_packet(sync1)) {
        ctx->serial_transfer.deferred = 1;
        g.master_pending = true;
    }
}

void gb_serial_link_tick(struct GBContext *ctx) {
    if (g.mode == LINK_OFF)
        return;

    /* Once the recv thread has the socket up, fire our version + status from
     * the main thread (only sender). */
    if (g.connected && !g.initial_handshake_done) {
        send_initial_handshake(ctx);
    }

    BGBPacket pkt;
    while (inbox_pop(&pkt)) {
        switch (pkt.cmd) {
        case BGB_CMD_SYNC1: {
            /* Peer was master; we should be slave. Reply with SB if armed. */
            uint8_t our_byte = 0xFF;
            bool armed = gb_serial_take_slave_byte(ctx, &our_byte);
            BGBPacket sync2 = {
                BGB_CMD_SYNC2, our_byte, 0, 0, bgb_timestamp_for(ctx),
            };
            send_packet(sync2);
            if (armed) {
                gb_serial_complete_transfer(ctx, pkt.b2);
            }
            break;
        }
        case BGB_CMD_SYNC2: {
            /* Reply to our master sync1. */
            if (g.master_pending) {
                gb_serial_complete_transfer(ctx, pkt.b2);
                g.master_pending = false;
            }
            break;
        }
        case BGB_CMD_SYNC3: {
            /* Keepalive — echo back. */
            BGBPacket reply = {
                BGB_CMD_SYNC3, 1, 0, 0, bgb_timestamp_for(ctx),
            };
            send_packet(reply);
            break;
        }
        case BGB_CMD_STATUS: {
            BGBPacket reply = {
                BGB_CMD_STATUS, 0x01, 0, 0, bgb_timestamp_for(ctx),
            };
            send_packet(reply);
            break;
        }
        default:
            break;
        }
    }
}
