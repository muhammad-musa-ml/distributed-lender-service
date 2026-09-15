// bench_client.cpp -- closed-loop latency harness. Reports p50/p90/p99/p99.9/max.
//
// WHY CLOSED-LOOP (one request in flight per connection, wait for the reply, then send the
// next): it measures RESPONSE TIME, which is the number a latency-sensitive system is
// judged on. An open-loop flood measures throughput and then reports a "latency" that is
// mostly queueing delay of the load generator's own making -- the classic coordinated-
// omission error, where the harness stops sending while the server is slow and therefore
// never records the slow period.
//
// WHY PERCENTILES AND NOT A MEAN: the interesting failures here are bimodal. Nagle's
// algorithm interacting with delayed ACK does not make every request slightly slower, it
// makes a small fraction of them ~40ms slower. A mean hides that completely; p99.9 and the
// max are where it shows up, which is why they are reported and the mean is not.
//
// WARM-UP is discarded: the first requests pay for TCP slow-start, cold caches, and page
// faults on the buffers. Including them measures process startup, not steady state.
//
// EVERY response is VERIFIED (the server echoes the request's first 8 bytes, which carry
// the sequence number). A latency harness that does not check its replies can post
// beautiful numbers for a server that is dropping or mis-framing work.

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool send_all(int fd, const std::uint8_t* p, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        const ssize_t r = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (r > 0) { sent += static_cast<std::size_t>(r); continue; }
        if (r < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool recv_all(int fd, std::uint8_t* p, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r > 0) { got += static_cast<std::size_t>(r); continue; }
        if (r < 0 && errno == EINTR) continue;
        return false;  // 0 = peer closed, <0 = error
    }
    return true;
}

double pct(const std::vector<double>& sorted_us, double q) {
    if (sorted_us.empty()) return 0.0;
    // Nearest-rank on an already-sorted vector. Stated because "which percentile
    // definition" is a real source of disagreement between two harnesses.
    std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(sorted_us.size()));
    if (idx >= sorted_us.size()) idx = sorted_us.size() - 1;
    return sorted_us[idx];
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 9099;
    int nodelay = 1;
    std::uint64_t iters = 50000;
    std::uint64_t warmup = 5000;
    std::uint32_t payload = 64;
    // --split-send=1 writes the 4-byte length header and the payload as TWO separate
    // send() calls instead of one. This is the whole point of the Nagle experiment: in a
    // strict ping-pong there is never more than one unacknowledged small segment, so Nagle
    // has nothing to withhold and TCP_NODELAY measurably does NOTHING. The cliff needs
    // write-write-read -- the second small write is held until the first is ACKed, and the
    // peer's delayed-ACK timer is what supplies the delay. Splitting the send reproduces
    // that shape exactly, which turns "Nagle is bad for latency" from folklore into a
    // measurement with a stated precondition.
    int split_send = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--host=", 0) == 0) host = a.substr(7);
        else if (a.rfind("--port=", 0) == 0) port = std::atoi(a.c_str() + 7);
        else if (a.rfind("--nodelay=", 0) == 0) nodelay = std::atoi(a.c_str() + 10);
        else if (a.rfind("--iters=", 0) == 0) iters = std::strtoull(a.c_str() + 8, nullptr, 10);
        else if (a.rfind("--warmup=", 0) == 0) warmup = std::strtoull(a.c_str() + 9, nullptr, 10);
        else if (a.rfind("--split-send=", 0) == 0) split_send = std::atoi(a.c_str() + 13);
        else if (a.rfind("--payload=", 0) == 0)
            payload = static_cast<std::uint32_t>(std::strtoul(a.c_str() + 10, nullptr, 10));
        else {
            std::fprintf(stderr,
                         "usage: %s [--host=H] [--port=N] [--nodelay=0|1] [--iters=N]"
                         " [--warmup=N] [--payload=BYTES] [--split-send=0|1]\n", argv[0]);
            return 2;
        }
    }
    if (payload < 8) payload = 8;  // the first 8 bytes carry the sequence number

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "bad host: %s\n", host.c_str()); return 1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect"); return 1;
    }
    if (nodelay) {
        int v = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
    }

    std::vector<std::uint8_t> req(4 + payload, 0);
    const std::uint32_t nlen = ::htonl(payload);
    std::memcpy(req.data(), &nlen, 4);
    for (std::uint32_t i = 8; i < payload; ++i) req[4 + i] = static_cast<std::uint8_t>(i);

    std::uint8_t resp[4 + 16];
    std::vector<double> lat_us;
    lat_us.reserve(iters);

    std::uint64_t mismatches = 0;
    const std::uint64_t total = warmup + iters;
    const auto wall0 = std::chrono::steady_clock::now();

    for (std::uint64_t i = 0; i < total; ++i) {
        std::memcpy(req.data() + 4, &i, 8);  // sequence number -> echoed back

        const auto t0 = std::chrono::steady_clock::now();
        if (split_send) {
            // Two small writes before any read -- the shape Nagle actually penalises.
            if (!send_all(fd, req.data(), 4)) { std::perror("send hdr"); return 1; }
            if (!send_all(fd, req.data() + 4, req.size() - 4)) {
                std::perror("send body"); return 1;
            }
        } else if (!send_all(fd, req.data(), req.size())) {
            std::perror("send"); return 1;
        }
        if (!recv_all(fd, resp, sizeof(resp))) {
            std::fprintf(stderr, "connection closed at iteration %llu\n",
                         static_cast<unsigned long long>(i));
            return 1;
        }
        const auto t1 = std::chrono::steady_clock::now();

        std::uint64_t echoed = 0;
        std::memcpy(&echoed, resp + 4 + 8, 8);
        if (echoed != i) ++mismatches;

        if (i >= warmup) {
            lat_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
    }
    const auto wall1 = std::chrono::steady_clock::now();
    ::close(fd);

    const double wall_s = std::chrono::duration<double>(wall1 - wall0).count();
    std::sort(lat_us.begin(), lat_us.end());

    std::printf("closed-loop latency  host=%s:%d  nodelay=%d  payload=%uB  split_send=%d\n",
                host.c_str(), port, nodelay, payload, split_send);
    std::printf("iters=%llu (warmup %llu discarded)   wall=%.3fs   throughput=%.0f req/s\n",
                static_cast<unsigned long long>(iters),
                static_cast<unsigned long long>(warmup), wall_s,
                static_cast<double>(total) / wall_s);
    std::printf("echo mismatches: %llu  %s\n", static_cast<unsigned long long>(mismatches),
                mismatches == 0 ? "(every reply matched its request)"
                                : "** REPLIES DID NOT MATCH -- timings are meaningless **");
    if (lat_us.empty()) { std::printf("no samples\n"); return 1; }

    std::printf("  min    %10.2f us\n", lat_us.front());
    std::printf("  p50    %10.2f us\n", pct(lat_us, 0.50));
    std::printf("  p90    %10.2f us\n", pct(lat_us, 0.90));
    std::printf("  p99    %10.2f us\n", pct(lat_us, 0.99));
    std::printf("  p99.9  %10.2f us\n", pct(lat_us, 0.999));
    std::printf("  max    %10.2f us\n", lat_us.back());
    return mismatches == 0 ? 0 : 1;
}
