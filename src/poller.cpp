#include "poller.hpp"

#include <unistd.h>

#include <cerrno>

namespace kvnet {
namespace {
constexpr std::size_t kBatch = 256;  // sockets reported per wait
}  // namespace
}  // namespace kvnet

#ifdef __linux__

#include <sys/epoll.h>

namespace kvnet {

namespace {

std::uint32_t to_epoll(std::uint32_t interest) {
    std::uint32_t ev = 0;
    // EPOLLRDHUP tells us the peer closed its write side. That is read side
    // news, so it is reported as readable and the recv returning 0 is what
    // actually ends the conversation.
    if (interest & kRead) ev |= EPOLLIN | EPOLLRDHUP;
    if (interest & kWrite) ev |= EPOLLOUT;
    return ev;
}

bool ctl(int ep, int op, int fd, std::uint32_t interest) {
    epoll_event ev{};
    ev.events = to_epoll(interest);
    ev.data.fd = fd;
    return epoll_ctl(ep, op, fd, &ev) == 0;
}

}  // namespace

Poller::Poller() : fd_(epoll_create1(0)) {}

Poller::~Poller() {
    if (fd_ >= 0) close(fd_);
}

bool Poller::add(int fd, std::uint32_t interest) { return ctl(fd_, EPOLL_CTL_ADD, fd, interest); }
bool Poller::mod(int fd, std::uint32_t interest) { return ctl(fd_, EPOLL_CTL_MOD, fd, interest); }

bool Poller::del(int fd) { return epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr) == 0; }

bool Poller::wait(std::vector<Event>& out, int timeout_ms) {
    out.clear();
    epoll_event evs[kBatch];
    int n = epoll_wait(fd_, evs, static_cast<int>(kBatch), timeout_ms);
    if (n < 0) return errno == EINTR;  // a signal is not a failure
    for (int i = 0; i < n; ++i) {
        std::uint32_t f = 0;
        if (evs[i].events & (EPOLLIN | EPOLLRDHUP)) f |= kReadable;
        if (evs[i].events & EPOLLOUT) f |= kWritable;
        if (evs[i].events & (EPOLLERR | EPOLLHUP)) f |= kHangup;
        out.push_back(Event{evs[i].data.fd, f});
    }
    return true;
}

}  // namespace kvnet

#elif defined(__APPLE__)

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

namespace kvnet {

namespace {

// kqueue keeps one filter per direction, so a change list always carries both
// and enables or disables each one.
int set_filters(int kq, int fd, std::uint32_t interest, bool remove) {
    struct kevent ch[2];
    if (remove) {
        EV_SET(&ch[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&ch[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    } else {
        EV_SET(&ch[0], fd, EVFILT_READ, EV_ADD | ((interest & kRead) ? EV_ENABLE : EV_DISABLE), 0,
               0, nullptr);
        EV_SET(&ch[1], fd, EVFILT_WRITE, EV_ADD | ((interest & kWrite) ? EV_ENABLE : EV_DISABLE), 0,
               0, nullptr);
    }
    return kevent(kq, ch, 2, nullptr, 0, nullptr);
}

}  // namespace

Poller::Poller() : fd_(kqueue()) {}

Poller::~Poller() {
    if (fd_ >= 0) close(fd_);
}

bool Poller::add(int fd, std::uint32_t interest) { return set_filters(fd_, fd, interest, false) == 0; }
bool Poller::mod(int fd, std::uint32_t interest) { return set_filters(fd_, fd, interest, false) == 0; }

bool Poller::del(int fd) {
    set_filters(fd_, fd, 0, true);
    return true;  // a socket about to be closed is removed from kqueue anyway
}

bool Poller::wait(std::vector<Event>& out, int timeout_ms) {
    out.clear();
    struct kevent evs[kBatch];
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
    int n = kevent(fd_, nullptr, 0, evs, static_cast<int>(kBatch), &ts);
    if (n < 0) return errno == EINTR;
    for (int i = 0; i < n; ++i) {
        const int fd = static_cast<int>(evs[i].ident);
        std::uint32_t f = 0;
        if (evs[i].flags & EV_ERROR) {
            f |= kHangup;
        } else if (evs[i].filter == EVFILT_READ) {
            // EV_EOF here means the peer closed its write side, and there may
            // still be buffered bytes to read, so this stays a read.
            f |= kReadable;
        } else if (evs[i].filter == EVFILT_WRITE) {
            f |= kWritable;
            if (evs[i].flags & EV_EOF) f |= kHangup;  // peer is gone for good
        }
        // One socket can be reported once per filter. Merge them so the
        // server sees a single event per socket, like it does on Linux.
        bool merged = false;
        for (auto& e : out) {
            if (e.fd == fd) {
                e.flags |= f;
                merged = true;
                break;
            }
        }
        if (!merged) out.push_back(Event{fd, f});
    }
    return true;
}

}  // namespace kvnet

#else
#error "kvnet needs epoll (Linux) or kqueue (macOS or BSD)"
#endif
