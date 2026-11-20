"""Exercise the actual S10 MFC app against the configured MySQL schema."""

import ctypes as c
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
from contextlib import contextmanager

from tools.verify_s09 import capture, find_window, send, u, wait_for
from tools.verify_s10_integration import fault_proxy, request, table_lock


@contextmanager
def app_window(exe, out, env=None):
    with subprocess.Popen([str(Path(exe).resolve())], cwd=out, env=env) as process:
        try:
            hwnd = wait_for(lambda: find_window(process.pid, lambda wnd: bool(u.GetDlgItem(wnd, 1100))),
                            "S10 MFC window missing")
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


def verify(exe, output):
    out = Path(output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    original = Path("artifacts/S04/sample-inputs/images/train/OK/image_10002.png")
    local = out / "용접 검사 이미지.png"
    shutil.copyfile(original, local)
    u.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    with app_window(exe, out) as (process, hwnd):
        def control(identifier):
            return u.GetDlgItem(hwnd, identifier)

        def text(identifier):
            return control_text(hwnd, identifier)

        def click(identifier):
            send(control(identifier), 0xF5)

        wait_for(lambda: text(1106) == "DB 연결", "MySQL readiness missing")
        wait_for(lambda: ":33361" in text(1107), "TCP listener missing")
        choose_file(open_picker(process, hwnd), local)
        wait_for(lambda: text(1005) == str(local), "File path missing")
        click(1101)
        wait_for(lambda: text(1007) == "정상" and text(1106) == "저장 완료", "File result missing")
        wait_for(lambda: text(1012) == "최근 저장 이력", "File history missing")
        capture(hwnd, out / "01-file-stored.png")

        request_id = "s10-ui-" + uuid.uuid4().hex
        mysql = Path(".tools/mysql-8.4.13-winx64/bin/mysql.exe").resolve(strict=True)
        admin = [str(mysql), "--no-defaults", "-h", "127.0.0.1", "-P", os.environ["WELDSCOPE_MYSQL_PORT"],
                 "-u", "root", "--batch", "--skip-column-names", "--unbuffered",
                 os.environ["WELDSCOPE_MYSQL_DATABASE"]]
        with table_lock(admin):
            first = request(33361, request_id)
            assert first[0] == "STATUS" and first[-1] == "IN_PROGRESS", first
            assert request(33361, request_id) == first
            assert request(33361, request_id + "-busy")[-1] == "BUSY"
            wait_for(lambda: text(1106) == "저장 중", "The app did not show the occupied slot")
            assert not u.IsWindowEnabled(control(1101))

        def stored_result():
            frame = request(33361, request_id)
            return frame if frame[0] == "RESULT" else None

        result = wait_for(stored_result, "TCP result missing")
        assert result == ["RESULT", request_id, first[2], "FAIL"]
        wait_for(lambda: text(1007) == "불량" and text(1106) == "저장 완료", "TCP result display missing")
        wait_for(lambda: text(1012) == "최근 저장 이력", "TCP history missing")
        capture(hwnd, out / "02-device-stored.png")

        list_view = control(1104)
        send(list_view, 0x201, 1, (32 << 16) | 32)
        send(list_view, 0x202, 0, (32 << 16) | 32)
        click(1110)
        wait_for(lambda: "global-dark-ratio-v1" in text(1010), "History record did not reopen")
        capture(hwnd, out / "03-history-opened.png")
        send(list_view, 0x100, 0x28)
        send(list_view, 0x101, 0x28)
        click(1110)
        wait_for(lambda: text(1005) == str(local), "Stored local row did not reopen")
        original_bytes = local.read_bytes()
        try:
            local.unlink()
            click(1110)
            wait_for(lambda: text(1004) == "원본 누락", "Missing original status absent")
            assert text(1007) == "정상"
            local.write_bytes(original_bytes + b"changed")
            click(1110)
            wait_for(lambda: text(1004) == "원본 변경", "Changed original status absent")
            assert text(1007) == "정상"
        finally:
            local.write_bytes(original_bytes)

        corrupt = Path("src/WeldScope.App/res/samples/corrupt.png").resolve(strict=True)
        choose_file(open_picker(process, hwnd), corrupt)
        wait_for(lambda: text(1005) == str(corrupt), "Corrupt file selection missing")
        click(1101)
        wait_for(lambda: text(1007) == "처리 오류" and text(1106) == "저장 완료", "Corrupt verdict missing")
        wait_for(lambda: text(1012) == "최근 저장 이력", "Corrupt history missing")
        send(list_view, 0x201, 1, (32 << 16) | 32)
        send(list_view, 0x202, 0, (32 << 16) | 32)
        for _ in range(2):
            send(list_view, 0x100, 0x28)
            send(list_view, 0x101, 0x28)
        click(1110)
        wait_for(lambda: text(1005) == str(local), "Earlier normal history missing")
        assert text(1007) == "정상" and text(1009) == "완료"

        competing = "s10-ui-" + uuid.uuid4().hex
        picker = open_picker(process, hwnd)
        with table_lock(admin):
            assert request(33361, competing)[-1] == "IN_PROGRESS"
            choose_file(picker, local)
            wait_for(lambda: text(1009) == "준비" and text(1007) == "판정 대기",
                     "New selection was not accepted while TCP storage was pending")
        wait_for(lambda: u.IsWindowEnabled(control(1101)), "Selection was lost after TCP completion")
        assert request(33361, competing)[0] == "RESULT"
        assert text(1005) == str(local) and text(1007) == "판정 대기"
        print(f"S10 MFC checks passed, request_id={request_id} inspection_id={first[2]}")

    mysql_port = int(os.environ["WELDSCOPE_MYSQL_PORT"])
    for index, (query, forwarded, expected) in enumerate((
        (b"START TRANSACTION", False, "미저장"), (b"COMMIT", True, "저장 확인 필요"),
    ), start=4):
        with fault_proxy(mysql_port, query, forwarded) as (proxy, fired):
            with app_window(exe, out, dict(os.environ, WELDSCOPE_MYSQL_PORT=str(proxy))) as (process, hwnd):
                wait_for(lambda: control_text(hwnd, 1106) == "DB 연결", "MySQL readiness missing")
                choose_file(open_picker(process, hwnd), local)
                wait_for(lambda: control_text(hwnd, 1005) == str(local), "File selection missing before TCP")
                request_id = "s10-ui-" + uuid.uuid4().hex
                first = request(33361, request_id)
                assert first[-1] == "IN_PROGRESS", first
                wait_for(lambda: control_text(hwnd, 1106) == expected, "Save error status missing")
                assert fired.is_set() and control_text(hwnd, 1007) == "불량"
                assert request(33361, request_id + "-blocked")[-1] == "DB_UNAVAILABLE"
                capture(hwnd, out / f"0{index}-save-error.png")
                send(u.GetDlgItem(hwnd, 1108), 0xF5)
                wait_for(lambda: control_text(hwnd, 1106) == "DB 연결" and
                         control_text(hwnd, 1005) == str(local), "Save retry did not restore the selection")
                assert control_text(hwnd, 1007) == "판정 대기"
                assert request(33361, request_id) == ["RESULT", request_id, first[2], "FAIL"]
    print("S10 MFC recovery and original checks passed")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: python -m tools.verify_s10_gui APP_EXE OUTPUT_DIRECTORY")
    verify(*sys.argv[1:])
