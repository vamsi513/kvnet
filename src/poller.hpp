// A small readiness interface over epoll (Linux) and kqueue (macOS and BSD).
// The server loop only needs four things: register a socket, change what that
// socket is waiting for, drop it, and wait for the next batch of ready
// sockets. Keeping the flags separate from the platform constants means
// server.cpp holds no epoll or kqueue code at all.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kvnet {

// What a socket is waiting for.
constexpr std::uint32_t kRead = 1u << 0;
constexpr std::uint32_t kWrite = 1u << 1;

// What actually happened. A peer closing only its write side shows up as
// kReadable, not kHangup, so the server can still send the replies it owes.
constexpr std::uint32_t kReadable = 1u << 0;
constexpr std::uint32_t kWritable = 1u << 1;
constexpr std::uint32_t kHangup = 1u << 2;

struct Event {
    int fd;
    std::uint32_t flags;
};

class Poller {
public:
    Poller();
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    bool valid() const { return fd_ >= 0; }
    bool add(int fd, std::uint32_t interest);
    bool mod(int fd, std::uint32_t interest);
    bool del(int fd);

    // Waits up to timeout_ms, replacing out with the sockets that are ready.
    // Returns false only on a real error. A timeout is success with out empty.
    bool wait(std::vector<Event>& out, int timeout_ms);

private:
    int fd_ = -1;
};

}  // namespace kvnet
