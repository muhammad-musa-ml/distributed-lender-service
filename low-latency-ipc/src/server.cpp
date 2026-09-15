// server.cpp -- an epoll edge-triggered TCP server built to be MEASURED.
//
// PROTOCOL (deliberately trivial, because the protocol is not the point):
//   request  : [u32 length][length bytes payload]
//   response : [u32 length][length bytes payload]  -- payload is the FNV-1a 64-bit hash of
//                                                     the request payload, little-endian,
//                                                     followed by the request's first 8
//                                                     bytes echoed back.
// The echo lets the client match a response to the request it sent (it stamps a sequence
// number into the first 8 bytes), which is what makes per-request round-trip latency
// measurable rather than merely throughput. The hash exists so the server does a small,
// fixed, non-elidable amount of work per request -- without it the compiler and the kernel
// together measure an empty loop.
//
// DESIGN NOTES that are actually decisions, not description:
//
//   * EDGE-TRIGGERED (EPOLLET) epoll. Level-triggered would re-report a readable fd on
//     every wait, which is simpler and slower under load. Edge-triggered obliges us to
//     drain each fd until EAGAIN -- forgetting that is the classic ET bug and produces a
//     connection that silently stalls forever, so the drain loop below is load-bearing.
//
//   * NON-BLOCKING accept in a loop, also to EAGAIN. With many simultaneous connects, one
//     accept per epoll event leaves the backlog draining one connection per wakeup.
//
//   * TCP_NODELAY is a RUNTIME FLAG (--nodelay=0|1), not a constant, because it is the
//     second measured optimisation -- and the measurement CORRECTED the claim that was
//     originally written here. The first draft of this comment asserted that Nagle costs
//     a latency cliff "on a request/response workload". Measured on this machine, that is
//     FALSE for a strict ping-pong: 50,000 closed-loop requests came back p50 45.73us with
//     NODELAY=1 and p50 46.54us with it off -- indistinguishable. The reason is that Nagle
//     may only withhold a small segment while an earlier one is UNACKNOWLEDGED, and a
//     ping-pong never has two in flight, so there is nothing to withhold.
//
//     The cliff is real but it has a PRECONDITION: write-write-read. Two small writes
//     before a read means the second waits on the ACK of the first, and the peer's
//     delayed-ACK timer supplies the delay. bench_client --split-send=1 reproduces exactly
//     that shape by writing the 4-byte header and the payload separately, and the measured
//     result is p50 45.20us -> 43,999.88us, a ~973x increase, tightly clustered between
//     42.5ms and 47.9ms -- the tight spread is the signature of a fixed timer rather than
//     of contention. That pair of runs is why the client reports percentiles and max
//     rather than a mean, and it is the difference between repeating folklore and stating
//     a precondition you have tested.
//
//   * ONE buffer per connection, grown once and reused. No per-request allocation on the
//     hot path: malloc is not slow, but it is UNPREDICTABLE, and an allocator that decides
//     to touch the arena during the 99.9th request is exactly the tail this project exists
//     to talk about.
//
//   * Partial reads and writes are handled. A request can arrive split across TCP segments
//     and a response can partially drain when the socket buffer fills; both are normal at
//     load and both are where naive servers corrupt their stream.
//
// NOT DONE, stated so the README's claims stay inside what the code does: no TLS, no
// multi-threaded acceptor (one event loop; SO_REUSEPORT sharding would be the next step),
// no io_uring path (liburing is not installed in the dev environment; the kernel supports
// it and the event loop is structured so a second backend could be added behind the same
// interface).

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kMaxEvents = 256;
constexpr std::uint32_t kMaxFrame = 1u << 20;  // 1 MiB ceiling; a length header is attacker-controlled
constexpr std::size_t kInitialBuf = 4096;

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

std::uint64_t fnv1a(const std::uint8_t* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

struct Conn {
    int fd{-1};
    std::vector<std::uint8_t> in;    // accumulated, unparsed bytes
    std::vector<std::uint8_t> out;   // pending response bytes
    std::size_t out_sent{0};         // how much of `out` has drained
    std::uint64_t requests{0};

    explicit Conn(int f) : fd(f) {
        in.reserve(kInitialBuf);
        out.reserve(kInitialBuf);
    }
};

// Build one response into `out`. Keeps the frame format in exactly one place.
void append_response(std::vector<std::uint8_t>& out, const std::uint8_t* payload,
                     std::uint32_t len) {
    const std::uint64_t h = fnv1a(payload, len);
    std::uint8_t echo[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const std::uint32_t ncopy = len < 8 ? len : 8;
    std::memcpy(echo, payload, ncopy);

    const std::uint32_t body = 16;  // 8 hash + 8 echo
    const std::uint32_t nlen = ::htonl(body);
    const std::size_t base = out.size();
    out.resize(base + 4 + body);
    std::memcpy(out.data() + base, &nlen, 4);
    std::memcpy(out.data() + base + 4, &h, 8);
    std::memcpy(out.data() + base + 12, echo, 8);
}

// Try to push `out` to the socket. Returns false if the connection died.
bool flush_out(Conn& c, bool& want_write) {
    want_write = false;
    while (c.out_sent < c.out.size()) {
        const ssize_t n = ::send(c.fd, c.out.data() + c.out_sent,
                                 c.out.size() - c.out_sent, MSG_NOSIGNAL);
        if (n > 0) {
            c.out_sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            want_write = true;  // kernel buffer full: wait for EPOLLOUT
            break;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;  // real error
    }
    if (c.out_sent == c.out.size()) {
        c.out.clear();
        c.out_sent = 0;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int port = 9099;
    int nodelay = 1;
    int backlog = 1024;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--port=", 0) == 0) port = std::atoi(a.c_str() + 7);
        else if (a.rfind("--nodelay=", 0) == 0) nodelay = std::atoi(a.c_str() + 10);
        else if (a.rfind("--backlog=", 0) == 0) backlog = std::atoi(a.c_str() + 10);
        else {
            std::fprintf(stderr,
                         "usage: %s [--port=N] [--nodelay=0|1] [--backlog=N]\n", argv[0]);
            return 2;
        }
    }

    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);
    ::signal(SIGPIPE, SIG_IGN);  // we use MSG_NOSIGNAL, but belt and braces

    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { std::perror("socket"); return 1; }

    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
    addr.sin_port = ::htons(static_cast<std::uint16_t>(port));
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind"); return 1;
    }
    if (::listen(listen_fd, backlog) < 0) { std::perror("listen"); return 1; }
    if (!set_nonblocking(listen_fd)) { std::perror("fcntl listen"); return 1; }

    const int ep = ::epoll_create1(0);
    if (ep < 0) { std::perror("epoll_create1"); return 1; }

    epoll_event ev{};
    ev.events = EPOLLIN;  // level-triggered on the LISTENER is fine and simpler
    ev.data.fd = listen_fd;
    if (::epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        std::perror("epoll_ctl listen"); return 1;
    }

    std::fprintf(stderr, "listening on 0.0.0.0:%d  TCP_NODELAY=%d  backlog=%d (edge-triggered)\n",
                 port, nodelay, backlog);

    std::unordered_map<int, Conn> conns;
    std::vector<epoll_event> events(kMaxEvents);
    std::uint64_t total_requests = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        const int n = ::epoll_wait(ep, events.data(), kMaxEvents, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            // Bind once: epoll_wait returns a signed count but the vector indexes with
            // size_t, and scattering casts through the body is how the wrong event gets
            // read during a later edit.
            const epoll_event& evt = events[static_cast<std::size_t>(i)];
            const int fd = evt.data.fd;

            // ---- new connections -------------------------------------------------
            if (fd == listen_fd) {
                for (;;) {  // drain the backlog
                    const int cfd = ::accept(listen_fd, nullptr, nullptr);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        std::perror("accept");
                        break;
                    }
                    if (!set_nonblocking(cfd)) { ::close(cfd); continue; }
                    if (nodelay) {
                        int v = 1;
                        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
                    }
                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    if (::epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                        std::perror("epoll_ctl add"); ::close(cfd); continue;
                    }
                    conns.emplace(cfd, Conn{cfd});
                }
                continue;
            }

            // ---- existing connection ---------------------------------------------
            auto it = conns.find(fd);
            if (it == conns.end()) continue;
            Conn& c = it->second;

            bool dead = (evt.events & (EPOLLHUP | EPOLLERR)) != 0;

            if (!dead && (evt.events & EPOLLOUT)) {
                bool want_write = false;
                if (!flush_out(c, want_write)) dead = true;
                if (!dead && !want_write) {
                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    cev.data.fd = fd;
                    ::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &cev);
                }
            }

            if (!dead && (evt.events & (EPOLLIN | EPOLLRDHUP))) {
                // EDGE-TRIGGERED: must drain to EAGAIN or this fd stalls forever.
                for (;;) {
                    std::uint8_t tmp[16384];
                    const ssize_t r = ::recv(c.fd, tmp, sizeof(tmp), 0);
                    if (r > 0) {
                        c.in.insert(c.in.end(), tmp, tmp + r);
                        continue;
                    }
                    if (r == 0) { dead = true; break; }          // peer closed
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    dead = true;
                    break;
                }

                // Parse as many complete frames as arrived.
                std::size_t off = 0;
                while (c.in.size() - off >= 4) {
                    std::uint32_t netlen = 0;
                    std::memcpy(&netlen, c.in.data() + off, 4);
                    const std::uint32_t len = ::ntohl(netlen);
                    if (len > kMaxFrame) { dead = true; break; }   // refuse absurd frames
                    if (c.in.size() - off - 4 < len) break;        // partial: wait for more
                    append_response(c.out, c.in.data() + off + 4, len);
                    off += 4 + len;
                    ++c.requests;
                    ++total_requests;
                }
                if (off > 0) {
                    c.in.erase(c.in.begin(), c.in.begin() + static_cast<std::ptrdiff_t>(off));
                }

                if (!dead && !c.out.empty()) {
                    bool want_write = false;
                    if (!flush_out(c, want_write)) {
                        dead = true;
                    } else if (want_write) {
                        epoll_event cev{};
                        cev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
                        cev.data.fd = fd;
                        ::epoll_ctl(ep, EPOLL_CTL_MOD, fd, &cev);
                    }
                }
            }

            if (dead) {
                ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                conns.erase(it);
            }
        }
    }

    std::fprintf(stderr, "\nshutting down: %llu request(s) served across %zu live connection(s)\n",
                 static_cast<unsigned long long>(total_requests), conns.size());
    for (auto& kv : conns) ::close(kv.first);
    ::close(listen_fd);
    ::close(ep);
    return 0;
}
