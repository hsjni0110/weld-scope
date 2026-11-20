"""Run the S06 sample device against the Windows console over loopback TCP."""

import argparse
import re
import select
import socket
import subprocess
import time
from contextlib import contextmanager
from pathlib import Path
from queue import Empty, Queue
from threading import Thread


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def request(request_id, source="weld-01", config="baseline-v1"):
    return f"INSPECT\t{request_id}\t{source}\t{config}\n".encode("ascii")


def result(reader, request_id):
    response = reader.readline(258)
    fields = response.split(b"\t")
    require(
        len(fields) == 4 and fields[0] == b"RESULT" and fields[1] == request_id.encode("ascii")
        and re.fullmatch(rb"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", fields[2]) and fields[3] == b"FAIL\n",
        f"Unexpected sample result: {response!r}",
    )
    return response, fields[2].decode("ascii")


def expect_error(reader, request_id, code):
    response = reader.readline(258)
    require(response == f"ERROR\t{request_id}\t{code}\n".encode("ascii"), f"Unexpected error: {response!r}")


class SampleServer:
    def __init__(self, process):
        self.process = process
        self.lines = Queue()
        self.observed = []
        self.reader = Thread(target=self.read_console)
        self.reader.start()

    def read_console(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)

    def wait_log(self, prefix):
        deadline = time.monotonic() + 5
        while True:
            try:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise Empty
                line = self.lines.get(timeout=remaining)
            except Empty as error:
                raise RuntimeError(f"Missing console output {prefix!r}. Last output: {self.observed[-5:]}") from error
            require(line is not None, f"Server exited before {prefix!r}: {self.observed[-5:]}")
            self.observed.append(line)
            if line.startswith(prefix):
                return line

    @contextmanager
    def connect(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=4) as connection:
            with connection.makefile("rb") as reader:
                yield connection, reader


@contextmanager
def run_server(executable):
    with subprocess.Popen(
        [executable, "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="ascii"
    ) as process:
        server = SampleServer(process)
        try:
            require(server.wait_log("mode=") == "mode=sample", "Expected a sample-only console")
            server.port = int(server.wait_log("port=").split("=", 1)[1])
            require(1 <= server.port <= 65535, "Invalid listener port")
            yield server
        finally:
            process.kill()
            process.wait(timeout=5)
            server.reader.join(timeout=5)


def verify_requests(server):
    with server.connect() as (connection, reader):
        first = request("req-001")
        connection.sendall(first[:-1])
        require(not select.select([connection], [], [], 0.15)[0], "A frame without LF produced a response")
        connection.sendall(first[-1:])
        original, inspection_id = result(reader, "req-001")
        print(f"mode=sample request={first.rstrip()!r} response={original.rstrip()!r}")

        connection.sendall(request("req-002") + request("req-003"))
        _, next_id = result(reader, "req-002")
        _, third_id = result(reader, "req-003")
        require(len({inspection_id, next_id, third_id}) == 3, "New requests must have distinct inspection IDs")
        print("scenario=split_and_combined status=ok")

    with server.connect() as (connection, reader):
        connection.sendall(first)
        require(result(reader, "req-001")[0] == original, "Reconnection changed the original response")
        for bad_request, request_id, code in (
            (request("req-001", source="unknown"), "req-001", "REQUEST_CONFLICT"),
            (request("req-001", config="unknown"), "req-001", "REQUEST_CONFLICT"),
            (request("unknown-source", source="unknown", config="unknown"), "unknown-source", "UNKNOWN_SOURCE"),
            (request("unknown-config", config="unknown"), "unknown-config", "UNKNOWN_CONFIG"),
        ):
            connection.sendall(bad_request)
            expect_error(reader, request_id, code)
        connection.sendall(first)
        require(result(reader, "req-001")[0] == original, "A conflict changed the original response")
    created = f"event=created request_id=req-001 inspection_id={inspection_id}"
    replayed = f"event=replayed request_id=req-001 inspection_id={inspection_id}"
    server.wait_log(replayed)
    server.wait_log(replayed)
    require(server.observed.count(created) == 1, "The original request was executed again")
    print(f"scenario=reconnect_replay_and_conflict status=ok inspection_id={inspection_id}")

    lost = request("req-unread")
    with server.connect() as (connection, _):
        connection.sendall(lost)
        log = server.wait_log("event=created request_id=req-unread inspection_id=")
    inspection_id = log.rsplit("=", 1)[1]
    with server.connect() as (connection, reader):
        connection.sendall(lost)
        require(result(reader, "req-unread")[1] == inspection_id, "Unread response changed the inspection ID")
    server.wait_log(f"event=replayed request_id=req-unread inspection_id={inspection_id}")
    require(server.observed.count(log) == 1, "The unread response caused a second inspection")
    print(f"scenario=unread_response status=ok inspection_id={inspection_id}")


def verify_invalid_connections(server):
    with server.connect() as (connection, _):
        partial = request("req-slow")[:-1]
        start = time.monotonic()
        connection.sendall(partial[:-8])
        for byte in partial[-8:]:
            connection.sendall(bytes([byte]))
            if select.select([connection], [], [], 0.30)[0]:
                break
        connection.settimeout(0.8)
        require(connection.recv(1) == b"", "Slow frame must close without a response")
        elapsed = time.monotonic() - start
        require(1.5 <= elapsed <= 3.2, f"Frame deadline moved with incoming bytes: {elapsed:.2f}s")
    server.wait_log("event=timeout operation=receive")
    print(f"scenario=receive_timeout status=ok elapsed_seconds={elapsed:.2f}")

    with server.connect() as (connection, reader):
        connection.sendall(request("req-incomplete")[:-1])
        connection.shutdown(socket.SHUT_WR)
        require(reader.read(1) == b"", "A partial frame at EOF produced a response")
    server.wait_log("event=incomplete_frame")

    for malformed in (request("req-oversized")[:-1] + b"x" * 257, b"INSPECT\tr\ts\tc\textra\n"):
        with server.connect() as (connection, reader):
            connection.sendall(malformed)
            expect_error(reader, "-", "BAD_FRAME")
            require(reader.read(1) == b"", "Malformed frame must close its connection")

    with server.connect() as (connection, reader):
        connection.sendall(request("req-recovery"))
        _, inspection_id = result(reader, "req-recovery")
        server.wait_log(f"event=created request_id=req-recovery inspection_id={inspection_id}")
        rejected_ids = ("req-slow", "req-incomplete", "req-oversized")
        for request_id in rejected_ids:
            prefix = f"event=created request_id={request_id} "
            require(not any(log.startswith(prefix) for log in server.observed), f"Invalid frame created {request_id}")
            connection.sendall(request(request_id))
            _, inspection_id = result(reader, request_id)
            server.wait_log(f"event=created request_id={request_id} inspection_id={inspection_id}")

        connection.sendall(request("req-half-close"))
        connection.shutdown(socket.SHUT_WR)
        result(reader, "req-half-close")
        require(reader.read(1) == b"", "Complete frame must receive its response before the socket closes")
    print("scenario=invalid_frame_recovery_and_half_close status=ok")


def verify_capacity(server):
    with server.connect() as (connection, reader):
        first = None
        for index in range(256):
            request_id = f"capacity-{index}"
            connection.sendall(request(request_id))
            response, _ = result(reader, request_id)
            if index == 0:
                first = response
        connection.sendall(request("capacity-256") + request("capacity-0"))
        expect_error(reader, "capacity-256", "CAPACITY")
        require(result(reader, "capacity-0")[0] == first, "Capacity rejected a retained request")
    print("scenario=capacity status=ok retained=256")


def verify_send_timeout(server):
    frame = request("req-backpressure")
    with socket.socket() as stalled:
        stalled.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        stalled.settimeout(1)
        stalled.connect(("127.0.0.1", server.port))
        try:
            # Repeat one request to fill the TCP send buffer without consuming history slots.
            stalled.sendall(frame * 32768)
        except TimeoutError:
            pass
        created = server.wait_log("event=created request_id=req-backpressure inspection_id=")
        server.wait_log("event=timeout operation=send")

    with server.connect() as (connection, reader):
        connection.sendall(frame)
        _, inspection_id = result(reader, "req-backpressure")
        require(created.rsplit("=", 1)[1] == inspection_id, "Send timeout changed the inspection ID")
    print(f"scenario=send_timeout status=ok inspection_id={inspection_id}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    executable = parser.parse_args().executable.resolve(strict=True)
    with run_server(executable) as server:
        verify_requests(server)
        verify_invalid_connections(server)
        verify_send_timeout(server)
    with run_server(executable) as server:
        verify_capacity(server)
    print("S06 checks passed")


if __name__ == "__main__":
    main()
