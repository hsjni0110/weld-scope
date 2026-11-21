"""Verify the S10 coordinator with real loopback TCP and a dedicated MySQL schema."""

import os
import select
import socket
import subprocess
import sys
import threading
import time
import uuid
from contextlib import ExitStack, contextmanager
from pathlib import Path

from tools.verify_s05_integration import read_packet


def wait_until(operation, predicate, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = operation()
        if predicate(value):
            return value
        time.sleep(0.04)
    raise RuntimeError(f"Timed out waiting for expected state, last value {value!r}")


@contextmanager
def host(program, env, file, *, config="config/baseline.json", device_image="src/WeldScope.App/res/samples/defect.png"):
    args = [program, config, device_image, file]
    with subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as process:
        try:
            port = int(process.stdout.readline())

            def command(text, response=True):
                process.stdin.write(text + "\n")
                process.stdin.flush()
                if response:
                    for line in process.stdout:
                        if not line.startswith("event="):
                            return line.rstrip()
                    raise RuntimeError(f"Test host closed while handling {text}")

            yield port, command
            command("quit", response=False)
            assert process.wait(timeout=12) == 0
        finally:
            if process.poll() is None:
                process.kill()


def request(port, request_id, source="weld-01", config="baseline-v1"):
    with socket.create_connection(("127.0.0.1", port), timeout=12) as connection:
        connection.sendall(f"INSPECT\t{request_id}\t{source}\t{config}\n".encode("ascii"))
        with connection.makefile("rb") as reply:
            return reply.readline().decode("ascii").rstrip("\n").split("\t")


@contextmanager
def table_lock(admin, mode="READ"):
    with subprocess.Popen(admin, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as locker:
        try:
            locker.stdin.write(f"LOCK TABLES inspections {mode}; SELECT 'locked';\n")
            locker.stdin.flush()
            assert locker.stdout.readline().strip() == "locked"
            yield
        finally:
            locker.communicate("UNLOCK TABLES;\n", timeout=8)


@contextmanager
def fault_proxy(mysql_port, target, forward_commit, after=None):
    with socket.create_server(("127.0.0.1", 0)) as listener:
        listener.settimeout(0.2)
        stopped = threading.Event()
        fired = threading.Event()
        errors = []

        def relay():
            while not stopped.is_set():
                try:
                    client, _ = listener.accept()
                except TimeoutError:
                    continue
                try:
                    with client, socket.create_connection(("127.0.0.1", mysql_port), timeout=8) as server:
                        client.settimeout(8)
                        eligible = after is None
                        while not stopped.is_set():
                            for source in select.select([client, server], [], [], 0.2)[0]:
                                packet = read_packet(source)
                                query = packet[5:].upper() if source is client and packet[3:5] == b"\0\3" else b""
                                if after is not None and query in (after, b"\0\1" + after):
                                    eligible = True
                                if eligible and not fired.is_set() and query in (target, b"\0\1" + target):
                                    if forward_commit:
                                        server.sendall(packet)
                                        assert read_packet(server)[4] == 0
                                    fired.set()
                                    raise EOFError
                                (server if source is client else client).sendall(packet)
                except (EOFError, ConnectionResetError, ConnectionAbortedError):
                    continue
                except Exception as error:
                    errors.append(error)
                    break

        worker = threading.Thread(target=relay, daemon=True)
        worker.start()
        try:
            yield listener.getsockname()[1], fired
        finally:
            stopped.set()
            worker.join(timeout=10)
            if errors:
                raise errors[0]


def verify(program, mysql):
    env = os.environ.copy()
    port = int(env["WELDSCOPE_MYSQL_PORT"])
    assert env["WELDSCOPE_MYSQL_HOST"] == "127.0.0.1" and not env.get("WELDSCOPE_MYSQL_CA_FILE")
    admin = [mysql, "--no-defaults", "-h", "127.0.0.1", "-P", str(port), "-u", "root",
             "--batch", "--skip-column-names", "--unbuffered", env["WELDSCOPE_MYSQL_DATABASE"]]
    normal = Path("artifacts/S04/sample-inputs/images/train/OK/image_10002.png").resolve(strict=True)
    corrupt = Path("src/WeldScope.App/res/samples/corrupt.png").resolve()

    def sql(statement):
        return subprocess.run(admin, input=statement, text=True, check=True,
                              capture_output=True, timeout=10).stdout.strip()

    def counts(inspection_id):
        return sql(f"SELECT (SELECT COUNT(*) FROM inspections WHERE inspection_id='{inspection_id}'), "
                   f"(SELECT COUNT(*) FROM analysis_attempts WHERE inspection_id='{inspection_id}');")

    with socket.socket() as unavailable:
        unavailable.bind(("127.0.0.1", 0))
        unavailable_port = unavailable.getsockname()[1]
        with host(program, dict(env, WELDSCOPE_MYSQL_PORT=str(unavailable_port)), normal) as (device, command):
            assert command("state").startswith("DISCONNECTED")
            assert command("inspect") == "BLOCKED"
            assert request(device, "s10-unavailable")[-1] == "DB_UNAVAILABLE"

    request_id = "s10-" + uuid.uuid4().hex
    with host(program, env, normal) as (device, command):
        assert command("inspect") == "ACCEPTED"
        file_result = wait_until(lambda: command("state").split(), lambda state: state[0] == "STORED")
        assert file_result[2:] == ["PASS", "1"], file_result
        assert counts(file_result[1]) == "1\t1"
        with table_lock(admin):
            first = request(device, request_id)
            assert first[0] == "STATUS" and first[-1] == "IN_PROGRESS", first
            assert request(device, request_id) == first
            assert request(device, request_id, "other")[-1] == "REQUEST_CONFLICT"
            full_id = request_id + "-full"
            assert request(device, full_id)[-1] == "BUSY"
            assert command("inspect") == "BLOCKED"
            wait_until(lambda: command("state"), lambda state: state.startswith("SAVING"), seconds=3)
            command("cancel", response=False)
            assert command("state").split()[0] == "SAVING"
        stored = wait_until(lambda: request(device, request_id), lambda frame: frame[0] == "RESULT")
        assert stored == ["RESULT", request_id, first[2], "FAIL"]
        assert counts(first[2]) == "1\t1"
        assert request(device, full_id, "other")[-1] == "UNKNOWN_SOURCE"
        assert sql(f"SELECT COUNT(*) FROM inspections WHERE request_id='{full_id}';") == "0"
        with socket.create_connection(("127.0.0.1", device), timeout=5) as unread:
            unread.sendall(f"INSPECT\t{request_id}\tweld-01\tbaseline-v1\n".encode("ascii"))
            unread.shutdown(socket.SHUT_WR)
        assert request(device, request_id) == stored
        with table_lock(admin, "WRITE"):
            acceptance = command("inspect")
            assert acceptance == "ACCEPTED", (acceptance, command("state"))
            wait_until(lambda: command("state"), lambda state: state.startswith("SAVING"), seconds=3)
            started = time.monotonic()
            assert request(device, request_id + "-blocked")[-1] == "BUSY"
            assert request(device, request_id + "-blocked2")[-1] == "BUSY"
            elapsed = time.monotonic() - started
            assert elapsed < 1.5, elapsed
            print(f"write_locked_lookup=BUSY two_replies_seconds={elapsed:.3f}")
    with host(program, env, corrupt) as (device, command):
        assert request(device, request_id) == stored
        assert request(device, request_id, config="other")[-1] == "REQUEST_CONFLICT"
        assert command("inspect") == "ACCEPTED"
        damaged = wait_until(lambda: command("state").split(), lambda state: state[0] == "STORED")
        assert damaged[2:] == ["ERROR", "0"]
        assert command("inspect_hold") == "ACCEPTED"
        assert command("wait_verdict") == "AT_VERDICT"
        command("cancel", response=False)
        command("release_verdict", response=False)
        cancelled = wait_until(lambda: command("state").split(), lambda state: state[0] == "STORED")
        assert sql(f"SELECT work_state, decision IS NULL FROM inspections WHERE inspection_id='{cancelled[1]}';") == "CANCELLED\t1"
    print("file=PASS tcp=FAIL corrupt=ERROR cancel=CANCELLED replay_after_restart=ok capacity=BUSY")

    for target, forwarded, expected in ((b"START TRANSACTION", False, "UNSAVED"), (b"COMMIT", True, "COMMIT_UNKNOWN")):
        request_id = "s10-" + uuid.uuid4().hex
        with fault_proxy(port, target, forwarded) as (proxy, fired):
            with host(program, dict(env, WELDSCOPE_MYSQL_PORT=str(proxy)), normal) as (device, command):
                first = request(device, request_id)
                failed = wait_until(lambda: command("state").split(), lambda state: state[0] == expected)
                assert fired.is_set() and failed[1:] == [first[2], "FAIL", "1"]
                assert command("inspect") == "BLOCKED"
                assert counts(first[2]) == ("1\t1" if forwarded else "0\t0")
                if forwarded:
                    assert request(device, request_id) == ["STATUS", request_id, first[2], expected]
                else:
                    command("retry", response=False)
                replay = wait_until(lambda: request(device, request_id), lambda frame: frame[0] == "RESULT")
                assert replay == ["RESULT", request_id, first[2], "FAIL"]
                assert counts(first[2]) == "1\t1"
        print(f"{expected}=recovery inspection_rows=1 analysis_rows=1")

    # A second failure cannot disprove a commit the server has already acknowledged.
    request_id = "s10-" + uuid.uuid4().hex
    with ExitStack() as app:
        with fault_proxy(port, b"COMMIT", True) as (proxy, fired):
            device, command = app.enter_context(host(program, dict(env, WELDSCOPE_MYSQL_PORT=str(proxy)), normal))
            first = request(device, request_id)
            unknown = wait_until(lambda: command("state"), lambda state: state.startswith("COMMIT_UNKNOWN"))
            assert fired.is_set() and counts(first[2]) == "1\t1"
        command("retry", response=False)
        # The post-command state can still be Saving if the retry thread is running.
        observed = wait_until(lambda: command("state"), lambda state: not state.startswith("SAVING"))
        assert observed == unknown and counts(first[2]) == "1\t1", observed
    print("commit_unknown_survives_failed_retry=ok")

    readonly = "s10-ro-" + uuid.uuid4().hex[:12]
    password = uuid.uuid4().hex
    sql(f"CREATE USER '{readonly}'@'localhost' IDENTIFIED BY '{password}'; "
        f"GRANT SELECT ON `{env['WELDSCOPE_MYSQL_DATABASE']}`.* TO '{readonly}'@'localhost';")
    try:
        subprocess.run([mysql, "--no-defaults", "--get-server-public-key", "-h", "127.0.0.1", "-P", str(port),
                        "-u", readonly, "-e", "SELECT 1"], env=dict(env, MYSQL_PWD=password),
                       check=True, capture_output=True, timeout=10)
        with host(program, dict(env, WELDSCOPE_MYSQL_USER=readonly, WELDSCOPE_MYSQL_PASSWORD=password), normal) as (_, command):
            assert command("state").startswith("DISCONNECTED")
            assert command("inspect") == "BLOCKED"
        print("read_only_account=blocked")
    finally:
        sql(f"DROP USER '{readonly}'@'localhost';")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: python tools/verify_s10_integration.py INTEGRATION_HOST MYSQL_CLIENT")
    verify(*sys.argv[1:])
    print("S10 coordinator checks passed")
