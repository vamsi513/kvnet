#!/usr/bin/env python3
"""Starts the server and checks real socket behavior: replies, partial writes
split across packets, pipelined requests, half close, oversized lines, rapid
connect and disconnect cycles, connection limits, and many simultaneous
clients."""
import socket
import subprocess
import sys
import threading
import time
import unittest

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/kvnet-server"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 17379
del sys.argv[1:]


def connect():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
    return s


def read_lines(s, n):
    buf = b""
    while buf.count(b"\r\n") < n:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf.decode().split("\r\n")[:n]


class KvnetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.proc = subprocess.Popen([BIN, str(PORT)], stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                connect().close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server did not start")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(timeout=5)

    def test_ping(self):
        with connect() as s:
            s.sendall(b"PING\r\n")
            self.assertEqual(read_lines(s, 1), ["+PONG"])

    def test_set_get(self):
        with connect() as s:
            s.sendall(b"SET k v1\r\nGET k\r\n")
            self.assertEqual(read_lines(s, 2), ["+OK", "$v1"])

    def test_missing_key_is_distinct_from_value_minus_one(self):
        with connect() as s:
            s.sendall(b"GET absent\r\nSET neg -1\r\nGET neg\r\n")
            self.assertEqual(read_lines(s, 3), ["*-1", "+OK", "$-1"])

    def test_pipelined_requests(self):
        with connect() as s:
            s.sendall(b"SET a 1\r\nSET b 2\r\nDEL a\r\nGET b\r\n")
            self.assertEqual(read_lines(s, 4), ["+OK", "+OK", ":1", "$2"])

    def test_bare_newline_accepted(self):
        with connect() as s:
            s.sendall(b"PING\n")
            self.assertEqual(read_lines(s, 1), ["+PONG"])

    def test_request_split_across_writes(self):
        with connect() as s:
            s.sendall(b"SET split ab")
            time.sleep(0.2)
            s.sendall(b"cd\r\nGET split\r\n")
            self.assertEqual(read_lines(s, 2), ["+OK", "$abcd"])

    def test_oversized_line_closes_connection_but_not_server(self):
        with connect() as s:
            try:
                s.sendall(b"x" * 10000)
            except OSError:
                pass
            s.settimeout(2)
            try:
                data = s.recv(100)
            except (ConnectionResetError, socket.timeout):
                data = b""
            self.assertEqual(data, b"")  # closed without a reply
        with connect() as s:  # server still serves new clients
            s.sendall(b"PING\r\n")
            self.assertEqual(read_lines(s, 1), ["+PONG"])

    def test_half_close_still_gets_all_replies(self):
        # A client that pipelines requests, shuts down its write side, then
        # reads must still receive every reply it is owed.
        with connect() as s:
            s.sendall(b"SET h 1\r\nGET h\r\nINCR h\r\nPING\r\n")
            s.shutdown(socket.SHUT_WR)
            self.assertEqual(read_lines(s, 4), ["+OK", "$1", ":2", "+PONG"])

    def test_half_close_under_load_keeps_all_replies(self):
        # With many clients at once the server reads a client's requests and
        # its FIN in the same recv, which is the case that used to drop the
        # queued replies on the floor.
        lost = []

        def worker(i):
            try:
                with connect() as s:
                    s.sendall(b"SET p%d 1\r\nGET p%d\r\nPING\r\n" % (i, i))
                    s.shutdown(socket.SHUT_WR)
                    got = read_lines(s, 3)
                    if got != ["+OK", "$1", "+PONG"]:
                        lost.append((i, got))
            except Exception as e:  # noqa: BLE001
                lost.append((i, repr(e)))

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(150)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(lost, [])

    def test_client_that_disconnects_midrequest(self):
        s = connect()
        s.sendall(b"SET half ")
        s.close()
        with connect() as s2:
            s2.sendall(b"PING\r\n")
            self.assertEqual(read_lines(s2, 1), ["+PONG"])

    def test_rapid_connect_disconnect_cycles(self):
        # Connections that open and close without sending anything must not
        # disturb the accept loop or leak slots.
        for _ in range(300):
            connect().close()
        with connect() as s:
            s.sendall(b"PING\r\n")
            self.assertEqual(read_lines(s, 1), ["+PONG"])

    def test_many_simultaneous_clients(self):
        errors = []

        def worker(i):
            try:
                with connect() as s:
                    s.sendall(f"SET c{i} {i}\r\nGET c{i}\r\n".encode())
                    got = read_lines(s, 2)
                    if got != ["+OK", f"${i}"]:
                        errors.append((i, got))
            except Exception as e:  # noqa: BLE001
                errors.append((i, repr(e)))

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(200)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])


class ConnectionLimitTest(unittest.TestCase):
    """Runs its own server with a small cap, so the limit is cheap to reach."""

    PORT = PORT + 1
    LIMIT = 5

    @classmethod
    def setUpClass(cls):
        cls.proc = subprocess.Popen(
            [BIN, str(cls.PORT), str(cls.LIMIT)], stderr=subprocess.DEVNULL
        )
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", cls.PORT), timeout=3).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("capped server did not start")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(timeout=5)

    def test_limit_pauses_accepting_then_resumes(self):
        held = []
        try:
            for _ in range(self.LIMIT):
                s = socket.create_connection(("127.0.0.1", self.PORT), timeout=3)
                s.sendall(b"PING\r\n")
                self.assertEqual(read_lines(s, 1), ["+PONG"])
                held.append(s)

            # The server is at its cap, so one more client waits in the
            # backlog and is not served yet.
            extra = socket.create_connection(("127.0.0.1", self.PORT), timeout=3)
            held.append(extra)
            extra.sendall(b"PING\r\n")
            extra.settimeout(1)
            with self.assertRaises((socket.timeout, TimeoutError)):
                extra.recv(100)

            # Closing one of the accepted clients frees a slot, and the
            # waiting client is then served.
            held.pop(0).close()
            extra.settimeout(3)
            self.assertEqual(read_lines(extra, 1), ["+PONG"])
        finally:
            for s in held:
                try:
                    s.close()
                except OSError:
                    pass


if __name__ == "__main__":
    unittest.main(verbosity=2)
