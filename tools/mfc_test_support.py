"""Native Windows UI, TCP, and MySQL helpers for integration checks."""

import ctypes as c
from ctypes import wintypes as w
from contextlib import contextmanager
import os
from pathlib import Path
import select
import socket
import subprocess
import threading
import time
import uuid
from PIL import Image

from tools.verify_s05_integration import read_packet

VERDICTS = {"PASS": "정상", "FAIL": "불량", "REVIEW": "검토 필요", "ERROR": "처리 오류"}

u = c.WinDLL("user32", use_last_error=True)
g = c.WinDLL("gdi32", use_last_error=True)
u.GetDlgItem.argtypes = [w.HWND, c.c_int]
u.GetDlgItem.restype = w.HWND
u.IsWindowEnabled.argtypes = [w.HWND]
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetDpiForWindow.argtypes = [w.HWND]
u.GetScrollPos.argtypes = [w.HWND, c.c_int]
u.GetDC.argtypes = [w.HWND]
u.GetDC.restype = w.HDC
u.ReleaseDC.argtypes = [w.HWND, w.HDC]
u.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
g.CreateCompatibleDC.argtypes = [w.HDC]
g.CreateCompatibleDC.restype = w.HDC
g.CreateCompatibleBitmap.argtypes = [w.HDC, c.c_int, c.c_int]
g.CreateCompatibleBitmap.restype = w.HBITMAP
g.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
g.SelectObject.restype = w.HGDIOBJ
g.DeleteObject.argtypes = [w.HGDIOBJ]
g.DeleteDC.argtypes = [w.HDC]

class BitmapInfoHeader(c.Structure):
    _fields_ = [("size", w.DWORD), ("width", w.LONG), ("height", w.LONG),
                ("planes", w.WORD), ("bits", w.WORD), ("compression", w.DWORD),
                ("image_size", w.DWORD), ("xppm", w.LONG), ("yppm", w.LONG),
                ("colors", w.DWORD), ("important", w.DWORD)]

g.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, c.c_void_p, c.POINTER(BitmapInfoHeader), w.UINT]

def capture(hwnd, path):
    bounds = w.RECT()
    u.GetWindowRect(hwnd, c.byref(bounds))
    width, height = bounds.right - bounds.left, bounds.bottom - bounds.top
    screen = u.GetDC(hwnd)
    memory = g.CreateCompatibleDC(screen)
    bitmap = g.CreateCompatibleBitmap(screen, width, height)
    previous = g.SelectObject(memory, bitmap)
    try:
        if not u.PrintWindow(hwnd, memory, 2):
            raise RuntimeError("PrintWindow failed")
        g.SelectObject(memory, previous)
        header = BitmapInfoHeader(c.sizeof(BitmapInfoHeader), width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
        data = c.create_string_buffer(width * height * 4)
        if g.GetDIBits(memory, bitmap, 0, height, data, c.byref(header), 0) != height:
            raise RuntimeError("GetDIBits failed")
        Image.frombytes("RGB", (width, height), data.raw, "raw", "BGRX").save(path)
    finally:
        g.SelectObject(memory, previous)
        g.DeleteObject(bitmap)
        g.DeleteDC(memory)
        u.ReleaseDC(hwnd, screen)


u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT, w.UINT, c.POINTER(c.c_size_t)]
u.SendMessageTimeoutW.restype = c.c_ssize_t
u.GetWindow.argtypes = [w.HWND, w.UINT]
u.GetWindow.restype = w.HWND
u.FindWindowExW.argtypes = [w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR]
u.FindWindowExW.restype = w.HWND
u.GetGUIThreadInfo.argtypes = [w.DWORD, c.c_void_p]


class GuiThreadInfo(c.Structure):
    _fields_ = [("size", w.DWORD), ("flags", w.DWORD)] + [
        (name, w.HWND) for name in ("active", "focus", "capture", "menu", "move", "caret")
    ] + [("rect", w.RECT)]



def send(window, message, first=0, second=0):
    result = c.c_size_t()
    if not u.SendMessageTimeoutW(window, message, first, second, 0x2, 500, c.byref(result)):
        raise RuntimeError(f"Window message {message:#x} timed out or failed")
    return result.value


def wait_for(predicate, label):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)
    raise AssertionError(label)


def find_window(pid, predicate):
    found = None

    @c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def visit(window, _):
        nonlocal found
        owner = w.DWORD()
        u.GetWindowThreadProcessId(window, c.byref(owner))
        if owner.value == pid and u.IsWindowVisible(window) and predicate(window):
            found = window
            return False
        return True

    u.EnumWindows(visit, 0)
    return found


@contextmanager
def app_window(exe, out, env=None):
    with subprocess.Popen([str(Path(exe).resolve())], cwd=out, env=env) as process:
        try:
            hwnd = wait_for(lambda: find_window(process.pid, lambda wnd: bool(u.GetDlgItem(wnd, 1100))),
                            "MFC window missing")
            yield process, hwnd
            u.PostMessageW(hwnd, 0x10, 0, 0)
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()


def control_text(hwnd, identifier):
    buffer = c.create_unicode_buffer(32768)
    send(u.GetDlgItem(hwnd, identifier), 0xD, len(buffer), c.addressof(buffer))
    return buffer.value


def open_picker(process, hwnd):
    u.PostMessageW(hwnd, 0x111, 1100, u.GetDlgItem(hwnd, 1100))
    return wait_for(lambda: find_window(process.pid, lambda wnd: u.GetWindow(wnd, 4) == hwnd),
                    "Native file dialog missing")


def choose_file(picker, path):
    filename = u.GetDlgItem(picker, 0x480)
    if not filename:
        combo = u.GetDlgItem(picker, 0x47C)
        filename = u.FindWindowExW(combo, None, "Edit", None) or u.FindWindowExW(
            u.FindWindowExW(combo, None, "ComboBox", None), None, "Edit", None)
    filename_text = c.create_unicode_buffer(str(path))
    send(filename, 0xC, 0, c.addressof(filename_text))
    u.PostMessageW(picker, 0x111, 1, u.GetDlgItem(picker, 1))


def select_first_history_row(list_view):
    # List view drag detection can wait for a mouse-up from within mouse-down.
    position = (32 << 16) | 32
    u.PostMessageW(list_view, 0x201, 1, position)
    u.PostMessageW(list_view, 0x202, 0, position)
    wait_for(lambda: send(list_view, 0x100C, -1, 2) == 0, "First history row missing")


def wait_until(operation, predicate, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = operation()
        if predicate(value):
            return value
        time.sleep(0.04)
    raise RuntimeError(f"Timed out waiting for expected state, last value {value!r}")


def request(port, request_id, source="weld-01", config="pooled-smooth-220-v2"):
    with socket.create_connection(("127.0.0.1", port), timeout=12) as connection:
        connection.sendall(f"INSPECT\t{request_id}\t{source}\t{config}\n".encode("ascii"))
        with connection.makefile("rb") as reply:
            return reply.readline().decode("ascii").rstrip("\n").split("\t")


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


def read_database():
    if os.environ["WELDSCOPE_MYSQL_HOST"] != "127.0.0.1":
        raise ValueError("Use the dedicated loopback MySQL instance")
    mysql = Path(".tools/mysql-8.4.13-winx64/bin/mysql.exe").resolve(strict=True)
    schema = os.environ["WELDSCOPE_MYSQL_DATABASE"]
    admin = [str(mysql), "--no-defaults", "--protocol=TCP", "--host=127.0.0.1",
             "--port=" + os.environ["WELDSCOPE_MYSQL_PORT"], "--user=root",
             "--batch", "--skip-column-names", schema]

    def query(sql):
        result = subprocess.run(admin, input=sql, text=True, check=True, capture_output=True, timeout=10)
        return [line.split("\t") for line in result.stdout.splitlines()]

    return query


def completed_request(request_id, config_id, port=33361):
    start = time.perf_counter()
    first = wait_until(lambda: request(port, request_id, config=config_id), lambda response: response[-1] != "BUSY")
    assert first[0] == "STATUS" and first[-1] == "IN_PROGRESS", first
    result = wait_until(lambda: request(port, request_id, config=config_id), lambda response: response[0] == "RESULT")
    assert result[2] == first[2], result
    return result, (time.perf_counter() - start) * 1000


def check_record(query, inspection_id, config_hash, image_hash, decision, version, *, include_time=False):
    inspection_id = str(uuid.UUID(inspection_id))
    rows = query("SELECT config_sha256, source_sha256, decision, algorithm_version "
                 f"FROM inspections WHERE inspection_id = '{inspection_id}';")
    assert rows == [[config_hash, image_hash, decision, version]], rows
    stages = query("SELECT stage, score, decision, reason, duration_ms FROM analysis_attempts "
                   f"WHERE inspection_id = '{inspection_id}' ORDER BY sequence_no;")
    assert stages and all(float(row[-1]) >= 0 for row in stages), stages
    return stages if include_time else [row[:-1] for row in stages]
