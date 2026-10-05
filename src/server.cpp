// Single-threaded event loop server for the kvnet protocol. The readiness
// mechanism lives in poller.cpp, which uses epoll on Linux and kqueue on
// macOS, so this file is the same on both. Non-blocking sockets,
// per-connection input and output buffers, line framing that tolerates partial
// reads, and caps on line length, pending output, and concurrent connections.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "poller.hpp"
#include "protocol.hpp"

namespace {

constexpr std::size_t kMaxPendingOut = 1 << 20;  // drop clients that never read replies
constexpr std::size_t kDefaultMaxConns = 1024;   // bounds file descriptors and memory
volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Conn {
    std::string in;
    std::string out;
    bool peer_done = false;       // peer closed its write side, so no more requests
    std::uint32_t interest = 0;   // what this socket is currently waiting for
};

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Keeps the registration in step with what this connection still needs. Once
// the peer is done sending we stop waiting to read, otherwise the end of file
// would keep the socket readable and spin the loop.
void sync_interest(kvnet::Poller& poller, int fd, Conn& c) {
    std::uint32_t want = c.peer_done ? 0u : kvnet::kRead;
    if (!c.out.empty()) want |= kvnet::kWrite;
    if (want == c.interest) return;
    if (poller.mod(fd, want)) c.interest = want;
}

// Starts and stops waiting on the listening socket. A full server must stop
// waiting on it, because a readable listener that is never accepted from would
// wake the loop on every pass and spin the CPU.
void set_accepting(kvnet::Poller& poller, int lfd, bool on, bool& accepting) {
    if (on == accepting) return;
    if (poller.mod(lfd, on ? kvnet::kRead : 0u)) accepting = on;
}

// Returns false if the connection should be closed.
bool flush(kvnet::Poller& poller, int fd, Conn& c) {
    while (!c.out.empty()) {
        ssize_t n = send(fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.out.erase(0, static_cast<std::size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    sync_interest(poller, fd, c);
    return true;
}

// Returns false if the connection should be closed.
bool handle_read(kvnet::Poller& poller, int fd, Conn& c, kvnet::Store& store) {
    char buf[8192];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n > 0) {
            c.in.append(buf, static_cast<std::size_t>(n));
            std::size_t start = 0, nl;
            while ((nl = c.in.find('\n', start)) != std::string::npos) {
                std::size_t end = nl;
                if (end > start && c.in[end - 1] == '\r') --end;
                if (end - start > kvnet::kMaxLine) return false;
                c.out += store.execute(c.in.substr(start, end - start));
                start = nl + 1;
            }
            c.in.erase(0, start);
            if (c.in.size() > kvnet::kMaxLine) return false;   // line without newline too long
            if (c.out.size() > kMaxPendingOut) return false;   // client is not reading
        } else if (n == 0) {
            c.peer_done = true;  // still allowed to send the replies we owe
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return flush(poller, fd, c);
}

}  // namespace

int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 7379;
    int max_arg = argc > 2 ? std::atoi(argv[2]) : static_cast<int>(kDefaultMaxConns);
    if (port <= 0 || port > 65535 || max_arg <= 0) {
        std::fprintf(stderr, "usage: %s [port] [max-connections]\n", argv[0]);
        return 2;
    }
    const std::size_t max_conns = static_cast<std::size_t>(max_arg);

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { std::perror("socket"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) { std::perror("bind"); return 1; }
    if (listen(lfd, 1024) < 0) { std::perror("listen"); return 1; }
    if (!set_nonblocking(lfd)) { std::perror("fcntl"); return 1; }

    kvnet::Poller poller;
    if (!poller.valid()) { std::perror("poller"); return 1; }
    if (!poller.add(lfd, kvnet::kRead)) { std::perror("poller add"); return 1; }

    std::unordered_map<int, Conn> conns;
    bool accepting = true;
    kvnet::Store store;
    std::fprintf(stderr, "kvnet listening on 127.0.0.1:%d\n", port);

    std::vector<kvnet::Event> events;
    while (!g_stop) {
        if (!poller.wait(events, 500)) { std::perror("poller wait"); break; }
        for (const auto& e : events) {
            if (e.fd == lfd) {
                for (;;) {
                    if (conns.size() >= max_conns) {
                        set_accepting(poller, lfd, false, accepting);
                        break;
                    }
                    int cfd = accept(lfd, nullptr, nullptr);
                    if (cfd < 0) {
                        // Only EAGAIN means the backlog is drained. Anything
                        // else needs its own answer.
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR || errno == ECONNABORTED) continue;
                        if (errno == EMFILE || errno == ENFILE) {
                            // Out of descriptors. Stop waiting on the listener
                            // and try again once a connection closes.
                            std::fprintf(stderr, "kvnet: out of file descriptors, pausing accept\n");
                            set_accepting(poller, lfd, false, accepting);
                            break;
                        }
                        std::perror("accept");
                        break;
                    }
                    set_nonblocking(cfd);
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    if (!poller.add(cfd, kvnet::kRead)) { close(cfd); continue; }
                    conns[cfd].interest = kvnet::kRead;
                }
                continue;
            }
            auto it = conns.find(e.fd);
            if (it == conns.end()) continue;
            Conn& c = it->second;
            bool ok = true;
            // Read before reacting to a hangup: the last requests can be
            // delivered in the same event as the peer closing its side.
            if (e.flags & kvnet::kReadable) ok = handle_read(poller, e.fd, c, store);
            if (ok && (e.flags & kvnet::kWritable)) ok = flush(poller, e.fd, c);
            if (ok && (e.flags & kvnet::kHangup)) ok = false;
            if (ok && c.peer_done && c.out.empty()) ok = false;  // replies sent
            if (!ok) {
                poller.del(e.fd);
                close(e.fd);
                conns.erase(it);
                if (conns.size() < max_conns) set_accepting(poller, lfd, true, accepting);
            }
        }
    }
    for (auto& kv : conns) close(kv.first);
    close(lfd);
    return 0;
}
