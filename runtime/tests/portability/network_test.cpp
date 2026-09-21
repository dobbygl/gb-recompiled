// Real loopback peers exercise the public C APIs without SDL or a cartridge.
#include "gb_socket.h"
#include "gbrt.h"
#include "network_discovery.h"
#include "serial_link.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
static GBContext context{};
static bool slave_armed = false;
static unsigned completions = 0;
static uint8_t received = 0;
extern "C" bool gb_serial_take_slave_byte(GBContext *, uint8_t *byte) {
    *byte = slave_armed ? 0x7e : 0xff;
    const bool armed = slave_armed;
    slave_armed = false;
    return armed;
}
extern "C" void gb_serial_complete_transfer(GBContext *ctx, uint8_t byte) {
    ++completions;
    received = byte;
    ctx->serial_transfer.deferred = 0;
}
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <typename Predicate>
static void eventually(Predicate predicate, const char *message,
                       std::chrono::milliseconds budget = 2000ms) {
    const auto end = Clock::now() + budget;
    do {
        gb_serial_link_tick(&context);
        if (predicate())
            return;
        std::this_thread::sleep_for(2ms);
    } while (Clock::now() < end);
    throw std::runtime_error(message);
}
struct Socket {
    gb_net::Socket fd;
    explicit Socket(gb_net::Socket value) : fd(value) {
        require(fd != gb_net::invalid, "socket creation failed");
    }
    ~Socket() { gb_net::close(fd); }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
};
static sockaddr_in address(uint16_t port) {
    sockaddr_in value{};
    value.sin_family = AF_INET;
    value.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    value.sin_port = htons(port);
    return value;
}
static uint16_t bind_ephemeral(gb_net::Socket fd) {
    auto addr = address(0);
    require(bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "bind failed");
    gb_net::AddressLength length = sizeof(addr);
    require(getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &length) == 0,
            "getsockname failed");
    return ntohs(addr.sin_port);
}
static uint16_t free_port() {
    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    return bind_ephemeral(socket.fd);
}
using Packet = std::array<uint8_t, 8>;
static void send_bytes(gb_net::Socket fd, const uint8_t *bytes, size_t count) {
    while (count) {
        const int sent = gb_net::send(fd, bytes, count);
        require(sent > 0, "peer send failed");
        bytes += sent;
        count -= static_cast<size_t>(sent);
    }
}
static void send_packet(gb_net::Socket fd, Packet packet) {
    send_bytes(fd, packet.data(), packet.size());
}
static Packet receive_packet(gb_net::Socket fd) {
    Packet packet{};
    size_t filled = 0;
    eventually(
        [&] {
            if (gb_net::readable(fd, 0) <= 0)
                return false;
            const int got = gb_net::receive(fd, packet.data() + filled, packet.size() - filled);
            require(got > 0, "peer receive failed");
            filled += static_cast<size_t>(got);
            return filled == packet.size();
        },
        "packet receive timed out");
    return packet;
}
static void expect(gb_net::Socket fd, Packet packet) {
    require(receive_packet(fd) == packet, "wire packet mismatch");
}
static void handshake(gb_net::Socket fd) {
    expect(fd, {1, 1, 4, 0, 0, 0, 0, 0});
    expect(fd, {108, 1, 0, 0, 0x78, 0x56, 0x34, 0x12});
    // The worker must assemble fragmented TCP reads, not treat recv as a packet.
    const Packet version{1, 1, 4, 0, 0, 0, 0, 0};
    send_bytes(fd, version.data(), 3);
    std::this_thread::sleep_for(10ms);
    send_bytes(fd, version.data() + 3, 5);
    eventually([] { return gb_serial_link_is_ready(); }, "version handshake failed");
    require(std::strcmp(gb_serial_link_peer_ip(), "127.0.0.1") == 0, "peer address missing");
    send_packet(fd, {108, 1, 0, 0, 0, 0, 0, 0});
    expect(fd, {108, 1, 0, 0, 0x78, 0x56, 0x34, 0x12});
}
static void shutdown_link() {
    const auto started = Clock::now();
    gb_serial_link_shutdown();
    require(Clock::now() - started < 1500ms, "link shutdown blocked");
    gb_serial_link_shutdown();
    require(!gb_serial_link_is_active() && !gb_serial_link_is_ready(), "link still active");
    require(gb_serial_link_peer_ip()[0] == '\0', "stale peer address");
}
static void link_tests() {
    context.cycles = 0x2468acf0;
    for (int reconnect = 0; reconnect < 2; ++reconnect) {
        const uint16_t port = free_port();
        require(gb_serial_link_start_listen(port), "runtime listen failed");
        require(!gb_serial_link_start_listen(port), "duplicate start accepted");
        Socket peer(::socket(AF_INET, SOCK_STREAM, 0));
        auto addr = address(port);
        require(connect(peer.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0,
                "peer connect failed");
        handshake(peer.fd);
        const unsigned before = completions;
        gb_serial_link_on_serial_byte(&context, 0xa5);
        require(context.serial_transfer.deferred, "master did not defer");
        expect(peer.fd, {104, 0xa5, 0x81, 0, 0x78, 0x56, 0x34, 0x12});
        send_packet(peer.fd, {105, 0x42, 0, 0, 0, 0, 0, 0});
        eventually([&] { return completions == before + 1; }, "master completion missing");
        require(received == 0x42 && !context.serial_transfer.deferred, "master byte mismatch");
        slave_armed = true;
        send_packet(peer.fd, {104, 0x39, 0x81, 0, 0, 0, 0, 0});
        expect(peer.fd, {105, 0x7e, 0, 0, 0x78, 0x56, 0x34, 0x12});
        require(completions == before + 2 && received == 0x39, "slave completion mismatch");
        send_packet(peer.fd, {104, 0x99, 0x81, 0, 0, 0, 0, 0});
        expect(peer.fd, {105, 0xff, 0, 0, 0x78, 0x56, 0x34, 0x12});
        require(completions == before + 2, "unarmed slave mutated guest");
        send_packet(peer.fd, {106, 0, 0, 0, 0, 0, 0, 0});
        expect(peer.fd, {106, 1, 0, 0, 0x78, 0x56, 0x34, 0x12});
        send_packet(peer.fd, {109, 0, 0, 0, 0, 0, 0, 0});
        eventually([] { return !gb_serial_link_is_ready(); }, "disconnect ignored");
        shutdown_link();
    }
    // Outbound mode, rejected version, and partial packet cancellation.
    for (int scenario = 0; scenario < 3; ++scenario) {
        Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
        const uint16_t port = bind_ephemeral(listener.fd);
        require(listen(listener.fd, 1) == 0, "test listener failed");
        require(gb_serial_link_start_connect("127.0.0.1", port), "runtime connect failed");
        require(gb_net::readable(listener.fd, 1000) > 0, "connect not observed");
        Socket peer(accept(listener.fd, nullptr, nullptr));
        if (scenario == 0) {
            expect(peer.fd, {1, 1, 4, 0, 0, 0, 0, 0});
            expect(peer.fd, {108, 1, 0, 0, 0x78, 0x56, 0x34, 0x12});
            send_packet(peer.fd, {1, 9, 9, 0, 0, 0, 0, 0});
            eventually([] { return gb_serial_link_peer_ip()[0] == '\0'; }, "bad version accepted");
            require(!gb_serial_link_is_ready(), "bad version marked ready");
        } else {
            handshake(peer.fd);
            const uint8_t partial[] = {104, 0xa5, 0x81};
            send_bytes(peer.fd, partial, sizeof(partial));
            std::this_thread::sleep_for(10ms);
        }
        shutdown_link();
    }
    // Failed bind/connect leave a reusable object and balanced host resources.
    Socket occupied(::socket(AF_INET, SOCK_STREAM, 0));
    const uint16_t port = bind_ephemeral(occupied.fd);
    require(listen(occupied.fd, 1) == 0, "occupied listener failed");
    require(!gb_serial_link_start_listen(port), "occupied port unexpectedly bound");
    require(!gb_serial_link_start_connect("127.0.0.1", free_port()), "refused connect succeeded");
    require(!gb_serial_link_start_connect("invalid host name!", 8765), "invalid DNS succeeded");
    for (int attempt = 0; attempt < 4; ++attempt) {
        require(gb_serial_link_start_listen(free_port()), "restart after failure failed");
        shutdown_link(); // No peer: accept must be cancellable on Windows too.
    }
    std::puts("PASS: link wire protocol, transfers, reconnect, rejection and bounded shutdown");
}
static std::string saved_uuid, saved_nick;
static bool load_pref(const char *key, char *out, size_t size) {
    const auto &value = std::strcmp(key, "lan.uuid") == 0 ? saved_uuid : saved_nick;
    if (value.empty())
        return false;
    std::snprintf(out, size, "%s", value.c_str());
    return true;
}
static void save_pref(const char *key, const char *value) {
    (std::strcmp(key, "lan.uuid") == 0 ? saved_uuid : saved_nick) = value;
}
static void discovery_tests() {
    const GBLanPrefsHooks hooks{load_pref, save_pref};
    gb_lan_set_prefs_hooks(&hooks);
    gb_lan_init();
    require(saved_uuid.size() == 36 && saved_uuid[14] == '4', "UUID not persisted");
    const std::string identity = saved_uuid;
    gb_lan_set_self_nickname("Loopback host");
    gb_lan_set_advertised_port(32123);
    require(gb_lan_advertised_port() == 32123 && saved_nick == "Loopback host", "settings lost");
    Socket sender(::socket(AF_INET, SOCK_DGRAM, 0));
    auto addr = address(8766);
    std::array<uint8_t, 76> hello{};
    std::memcpy(hello.data(), "PG1!", 4);
    hello[4] = 1;
    hello[6] = 0x65;
    hello[7] = 0x87;
    const char uuid[] = "12345678-1234-4123-8123-123456789abc";
    std::memcpy(hello.data() + 8, uuid, 36);
    std::memcpy(hello.data() + 44, "Loopback peer", 13);
    auto send = [&](size_t length = 76) {
        require(sendto(sender.fd, reinterpret_cast<const char *>(hello.data()),
                       static_cast<int>(length), 0, reinterpret_cast<sockaddr *>(&addr),
                       sizeof(addr)) == static_cast<int>(length),
                "hello send failed");
    };
    GBLanPeer peers[32]{};
    for (int restart = 0; restart < 2; ++restart) {
        gb_lan_set_enabled(true);
        require(gb_lan_is_enabled(), "discovery did not start");
        gb_lan_set_enabled(true);
        send(20);
        hello[0] = '?';
        send();
        hello[0] = 'P';
        hello[4] = 2;
        send();
        hello[4] = 1;
        hello[8] = 1;
        send();
        hello[8] = '1';
        std::memcpy(hello.data() + 8, identity.data(), 36);
        send();
        std::this_thread::sleep_for(100ms);
        require(gb_lan_get_peers(peers, 32) == 0, "invalid/self hello accepted");
        std::memcpy(hello.data() + 8, uuid, 36);
        send();
        eventually([&] { return gb_lan_get_peers(peers, 32) == 1; }, "valid hello missing");
        require(std::strcmp(peers[0].uuid, uuid) == 0 && peers[0].bgb_port == 0x8765 &&
                    std::strcmp(peers[0].nickname, "Loopback peer") == 0 &&
                    std::strcmp(peers[0].ip, "127.0.0.1") == 0,
                "hello fields corrupted");
        hello[6] = 0x66;
        send();
        eventually([&] { return gb_lan_get_peers(peers, 32) == 1 && peers[0].bgb_port == 0x8766; },
                   "peer update missing");
        hello[6] = 0x65;
        require(gb_lan_get_peers(nullptr, 0) == 0, "zero capacity snapshot failed");
        for (int i = 0; i < 20; ++i) {
            gb_lan_set_advertised_port(static_cast<uint16_t>(32000 + i));
            gb_lan_set_self_nickname(i % 2 ? "Odd" : "Even");
        }
        if (restart == 0) {
            eventually([&] { return gb_lan_get_peers(peers, 32) == 0; },
                       "stale peer did not expire", 11000ms);
        }
        const auto started = Clock::now();
        gb_lan_shutdown();
        require(Clock::now() - started < 1500ms, "discovery shutdown blocked");
        require(!gb_lan_is_enabled() && gb_lan_get_peers(peers, 32) == 0, "stale discovery state");
        gb_lan_shutdown();
        gb_lan_init();
        require(identity == gb_lan_self_uuid() && saved_nick == gb_lan_self_nickname(),
                "identity changed on restart");
    }
    gb_lan_shutdown();
    gb_lan_set_prefs_hooks(nullptr);
    std::puts("PASS: discovery wire validation, identity, peer updates, expiry and restart");
}
int main(int argc, char **argv) {
    try {
        gb_net::Environment environment;
        require(environment.start(), "host network startup failed");
        if (argc == 2 && std::strcmp(argv[1], "discovery") == 0)
            discovery_tests();
        else
            link_tests();
        return 0;
    } catch (const std::exception &error) {
        gb_serial_link_shutdown();
        gb_lan_shutdown();
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
