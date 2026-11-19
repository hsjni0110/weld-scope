"""Verify overlapping saves and connection failures against dedicated loopback MySQL."""

import os
import select
import socket
import subprocess
import sys
import time
import uuid
from contextlib import ExitStack
from pathlib import Path


def read_bytes(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise EOFError("MySQL connection closed before the fault point")
        data.extend(chunk)
    return data


def read_packet(connection):
    header = read_bytes(connection, 4)
    return header + read_bytes(connection, int.from_bytes(header[:3], "little"))


def disconnect(listener, server_port, query, forward_commit):
    client, _ = listener.accept()
    with client, socket.create_connection(("127.0.0.1", server_port), timeout=10) as server:
        client.settimeout(10)
        while True:
            ready, _, _ = select.select([client, server], [], [], 10)
            if not ready:
                raise TimeoutError("MySQL exchange stalled before the fault point")
            for source in ready:
                packet = read_packet(source)
                if source is client:
                    # COM_QUERY can prefix SQL with zero attributes and one parameter set.
                    if packet[3:5] == b"\x00\x03" and packet[5:].upper() in (query, b"\x00\x01" + query):
                        if forward_commit:
                            server.sendall(packet)
                            if read_packet(server)[4] != 0:
                                raise RuntimeError("MySQL did not acknowledge COMMIT")
                            print("server_commit=acknowledged, client_reply=dropped")
                        else:
                            print("connected_session=dropped_before_transaction")
                        return
                    server.sendall(packet)
                else:
                    client.sendall(packet)


def expect(result, code, *lines):
    print(result.stdout, end="")
    print(result.stderr, end="")
    if result.returncode != code or not set(lines).issubset(result.stdout.splitlines()):
        raise RuntimeError(f"Expected exit {code} and {lines}, got exit {result.returncode}")
    return result.stdout


def verify_overlap(program, env, mysql_client):
    inspection = f"s05-overlap-{uuid.uuid4().hex}"
    save = [program, "save", inspection, f"{inspection}-r", "two-attempts"]
    admin = [
        mysql_client, "--no-defaults", "--protocol=TCP", "--host=127.0.0.1",
        f"--port={env['WELDSCOPE_MYSQL_PORT']}", "--user=root",
        f"--database={env.get('WELDSCOPE_MYSQL_DATABASE', 'weldscope_s05')}",
        "--unbuffered", "--batch", "--skip-column-names",
    ]
    admin_env = {key: value for key, value in env.items() if key != "MYSQL_PWD"}
    options = dict(text=True, capture_output=True, timeout=10)

    def sql(query):
        return subprocess.run(admin + ["-e", query], env=admin_env, check=True, **options).stdout.strip()

    processes = []
    with ExitStack() as stack:
        locker = stack.enter_context(
            subprocess.Popen(admin, env=admin_env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        )
        try:
            # Hold the table until both application connections reach the same database lock.
            locker.stdin.write("SET SESSION lock_wait_timeout=5; LOCK TABLES inspections WRITE; SELECT 'locked';\n")
            locker.stdin.flush()
            if locker.stdout.readline().strip() != "locked":
                raise RuntimeError("Could not lock the isolated inspections table")
            for _ in range(2):
                processes.append(stack.enter_context(subprocess.Popen(save, env=env, text=True, stdout=subprocess.PIPE)))
            deadline = time.monotonic() + 3
            while sql("SELECT COUNT(*) FROM information_schema.PROCESSLIST "
                      "WHERE DB = DATABASE() AND STATE = 'Waiting for table metadata lock'") != "2":
                if time.monotonic() >= deadline:
                    raise RuntimeError("Both saves did not reach the database lock")
                time.sleep(0.05)
            print("simultaneous_waiting_db_sessions=2")
            locker.communicate("UNLOCK TABLES;\n", timeout=5)
            if locker.returncode != 0:
                raise RuntimeError("Could not release the inspections table lock")

            statuses = []
            for process in processes:
                stdout, _ = process.communicate(timeout=10)
                if process.returncode != 0:
                    raise RuntimeError(f"Concurrent save failed: {stdout}")
                statuses.extend(line for line in stdout.splitlines() if line.startswith("status="))
            if sorted(statuses) != ["status=replay", "status=stored"]:
                raise RuntimeError(f"Concurrent saves did not converge: {statuses}")
            print(", ".join(sorted(statuses)))
        finally:
            for process in [locker, *processes]:
                if process.poll() is None:
                    process.kill()
                    process.wait()

    counts = sql(
        f"SELECT (SELECT COUNT(*) FROM inspections WHERE inspection_id = '{inspection}'), "
        f"(SELECT COUNT(*) FROM analysis_attempts WHERE inspection_id = '{inspection}')"
    )
    if counts != "1\t2":
        raise RuntimeError(f"Expected one inspection and two attempts, got {counts}")
    print("inspection_rows=1, attempt_rows=2")
    result = subprocess.run([program, "get", inspection], env=env, **options)
    output = expect(result, 0, "status=found", "attempt_count=2", "attempt.1.stage=BASELINE",
                    "attempt.2.stage=PRECISE", "attempt.2.score=NULL")
    if output.index("attempt.1.stage=") > output.index("attempt.2.stage="):
        raise RuntimeError("Attempt order was lost")


def verify(program, env, query, forward_commit):
    inspection = f"s05-fault-{uuid.uuid4().hex}"
    save = [program, "save", inspection, f"{inspection}-r"]
    get = [program, "get", inspection]
    options = dict(text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        proxy_env = dict(env, WELDSCOPE_MYSQL_PORT=str(listener.getsockname()[1]))
        with subprocess.Popen(save, env=proxy_env, **options) as process:
            try:
                disconnect(listener, int(env["WELDSCOPE_MYSQL_PORT"]), query, forward_commit)
                stdout, stderr = process.communicate(timeout=15)
            finally:
                if process.poll() is None:
                    process.kill()
            result = subprocess.CompletedProcess(save, process.returncode, stdout, stderr)

    def run(command):
        return subprocess.run(command, env=env, timeout=15, **options)

    if forward_commit:
        expect(result, 5, "status=commit_unknown", "decision=FAIL")
        before = expect(run(get), 0, "status=found", "attempt_count=1")
        expect(run(save), 0, "status=replay")
    else:
        expect(result, 1, "status=failed", "decision=FAIL")
        expect(run(get), 4, "status=not_found")
        expect(run(save), 0, "status=stored")
        before = expect(run(get), 0, "status=found", "attempt_count=1")
        expect(run(save), 0, "status=replay")
    if before != expect(run(get), 0, "status=found", "attempt_count=1"):
        raise RuntimeError("Replay changed the stored inspection")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: python tools/verify_s05_integration.py HISTORY_EXE MYSQL_EXE")
    env = os.environ.copy()
    if env.get("WELDSCOPE_MYSQL_HOST") != "127.0.0.1" or not env.get("WELDSCOPE_MYSQL_PORT"):
        raise SystemExit("Set WELDSCOPE_MYSQL_HOST=127.0.0.1 and the dedicated test server port")
    if env.get("WELDSCOPE_MYSQL_CA_FILE"):
        raise SystemExit("Clear WELDSCOPE_MYSQL_CA_FILE for the dedicated plaintext test server")
    program = str(Path(sys.argv[1]).resolve(strict=True))
    mysql_client = str(Path(sys.argv[2]).resolve(strict=True))
    expect(subprocess.run([program, "version"], env=env, text=True, capture_output=True, timeout=15), 0)
    verify_overlap(program, env, mysql_client)
    verify(program, env, b"START TRANSACTION", forward_commit=False)
    verify(program, env, b"COMMIT", forward_commit=True)
    print("S05 database integration checks passed")


if __name__ == "__main__":
    main()
