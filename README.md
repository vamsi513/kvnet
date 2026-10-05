# kvnet

A small in-memory key-value server over TCP, written in C++17 with a
non-blocking event loop, plus a load generator written in C. I built it to
practice network programming: non-blocking sockets, event loops, and framing a
text protocol over a byte stream. It uses epoll on Linux and kqueue on macOS.

It is a learning project, not a Redis replacement. See Limitations.

## Protocol

One request per line, terminated by `\n` or `\r\n`. Replies end in `\r\n`.

| Request | Reply |
|---|---|
| `PING` | `+PONG` |
| `SET key value` | `+OK` (the value may contain spaces) |
| `GET key` | `$value`, or `*-1` if the key is missing |
| `DEL key` | `:1` if deleted, `:0` if absent |
| `INCR key` | `:n` after incrementing; a missing key starts at 0 |
| anything invalid | `-ERR message` |

## Design

- `src/server.cpp`: single-threaded event loop with no platform code in it.
  Listening and client sockets are non-blocking. Each connection has an input
  buffer and an output buffer, and the server waits to write only while a
  client has unsent output.
- `src/poller.cpp`: the readiness layer behind a four call interface, add,
  modify, remove, and wait. epoll on Linux and kqueue on macOS, picked with
  `#ifdef __linux__` and `#ifdef __APPLE__`. A peer closing just its write side
  is reported as readable on both, which is what lets the server finish sending
  replies it already owes.
- Framing: requests are split on newlines from the input buffer, so a request
  that arrives in several packets, or several requests in one packet, both work.
- Limits: a line longer than 4096 bytes, or more than 1 MiB of unread replies,
  closes that connection. The server keeps running.
- Connection cap: 1024 by default, overridable with a second argument. At the
  cap the server stops watching the listening socket instead of spinning on it,
  and resumes once a connection closes. The same path handles running out of
  file descriptors.
- A client that closes its write side still receives the replies it is owed
  before the connection is dropped.
- `src/protocol.cpp`: parses and runs commands. It has no socket code, so it is
  unit tested directly.
- `bench/kvbench.c`: opens N connections (one thread each), sends alternating
  `SET` and `GET` requests one at a time, and reports throughput and latency
  percentiles.

## Build and test

```
make            # builds build/kvnet-server and build/kvbench
make test       # unit tests (C++) and socket tests (Python)
make sanitize   # rebuilds with AddressSanitizer and UBSan and reruns the tests
build/kvnet-server 7379          # port
build/kvnet-server 7379 4096     # port and max connections
```

Requires Linux or macOS, a C++17 compiler, a C compiler, make, and Python 3.
Tested on Ubuntu 24.04 with g++ 13 and on macOS 26.6 with Apple clang 21.

## Benchmark

100,000 requests per run, request and response one at a time per connection,
so each connection has at most one request in flight. Each configuration was
run three times and the median run is reported. Client and server run on the
same machine over loopback, so the client competes with the server for CPU.

Hardware: Apple M5, 10 cores, 16 GB RAM.

Native on macOS 26.6, Apple clang 21, no Docker:

| Connections | Throughput (req/s) | p50 (us) | p99 (us) |
|---|---|---|---|
| 1 | 76,301 | 13 | 19 |
| 10 | 252,298 | 41 | 63 |
| 100 | 249,621 | 391 | 755 |
| 500 | 248,246 | 1,950 | 2,153 |

In Docker, Ubuntu 24.04 with g++ 13.3, 10 CPUs given to the VM. Docker on macOS
runs Linux in a virtual machine, so these numbers include that overhead and are
not a measurement of Linux on bare metal:

| Connections | Throughput (req/s) | p50 (us) | p99 (us) |
|---|---|---|---|
| 1 | 47,078 | 20 | 32 |
| 10 | 206,584 | 39 | 92 |
| 100 | 215,461 | 448 | 901 |
| 500 | 212,878 | 2,327 | 2,596 |

No errors were reported in any run. Throughput flattens after about 10
connections because the server is single threaded, and from there added
concurrency shows up as latency rather than as more requests per second.

Treat these as relative numbers for this setup. They are not a comparison with
any other server, and they will differ on other hardware.

## Limitations

- Single thread, so one slow command delays every client.
- Linux and macOS only. There is no Windows backend.
- Data lives in memory only. No persistence, expiry, or eviction.
- Binds to 127.0.0.1 only. No authentication and no TLS.
- Values cannot contain newlines, and cannot be empty, since `SET key` with no
  value is rejected as a usage error. The protocol is not Redis compatible.
- `INCR` is a 64 bit signed counter and refuses to increment past its maximum
  rather than wrapping.
- The benchmark uses loopback, so it says nothing about real network latency or loss.

## Ideas for next steps

Edge-triggered readiness, a worker thread pool with sharded stores, a binary
protocol with length prefixes, and write-ahead logging for persistence.
