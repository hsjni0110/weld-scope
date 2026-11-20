"""Exercise the actual S09 Windows app and capture only its window."""
import argparse
import csv
import ctypes as c
import hashlib
from ctypes import wintypes as w
import json
from pathlib import Path
import shutil
import subprocess
import time
from PIL import Image, ImageChops, ImageStat

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

PICKER, START, STOP, CANDIDATE, HISTORY, IMAGE = range(1100, 1106)
METADATA, FILENAME, VERDICT, STATUS, DETAIL = 1004, 1005, 1007, 1009, 1010


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


def compare_cli(cli, config, image, output):
    subprocess.run([str(cli), "--image", str(image), "--config", str(config), "--output", str(output)],
                   capture_output=True, check=True)
    with (output / "results.csv").open(encoding="utf-8", newline="") as stream:
        row, = csv.DictReader(stream)
    assert row["image_sha256"] == hashlib.sha256(image.read_bytes()).hexdigest()
    return row


def check_displayed_image(screenshot, viewport, expected_png):
    with Image.open(screenshot) as frame, Image.open(expected_png) as expected:
        view = frame.crop(viewport).convert("RGB")
        background = Image.new("RGB", view.size, (19, 28, 41))
        image_bounds = ImageChops.difference(view, background).getbbox()
        assert image_bounds is not None, f"Image viewer is empty in {screenshot.name}"
        rendered = view.crop(image_bounds)
        scaled = expected.convert("RGB").resize(rendered.size, Image.Resampling.BILINEAR)
        error = sum(ImageStat.Stat(ImageChops.difference(rendered, scaled)).mean) / 3
        assert error <= 8, f"{screenshot.name} does not match CLI {expected_png.name} (mean error {error:.2f})"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=Path("artifacts/S09/ARM64/Release/WeldScope.App.exe"))
    parser.add_argument("--cli", type=Path, default=Path("artifacts/S04/build/Release/weldscope_inspect.exe"))
    parser.add_argument("--inputs", type=Path, default=Path("artifacts/S04/sample-inputs/images/train"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    exe, out = args.exe.resolve(), args.output.resolve()
    out.mkdir(parents=True)
    same_name = "용접 검사 이미지.png"
    passed = out / "한글 경로" / same_name
    failed = out / "다른 경로" / same_name
    for src, dest in ((args.inputs / "OK/image_10002.png", passed), (args.inputs / "KO/image_1.png", failed)):
        dest.parent.mkdir()
        shutil.copyfile(src, dest)
    corrupt = out / "손상.png"
    source_bytes = passed.read_bytes()
    corrupt.write_bytes(source_bytes[:-1] + bytes([source_bytes[-1] ^ 1]))
    missing = out / "missing.png"
    shutil.copyfile(passed, missing)
    oversized = out / "oversized.png"
    Image.new("RGB", (4001, 4000)).save(oversized)
    unsupported = out / "grayscale.png"
    Image.new("L", (2, 2)).save(unsupported)
    large = out / "최대 픽셀.png"
    Image.linear_gradient("L").resize((4000, 4000)).convert("RGB").save(large)
    assert large.stat().st_size < 16 * 1024 * 1024
    expected = {image: compare_cli(args.cli.resolve(), exe.with_name("baseline.json"), image, out / f"cli-{index}")
                for index, image in enumerate((passed, failed))}

    u.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    process = subprocess.Popen([str(exe)], cwd=out)
    checks = []
    try:
        hwnd = wait_for(lambda: find_window(process.pid, lambda wnd: bool(u.GetDlgItem(wnd, PICKER))), "MFC window missing")

        def control(control_id):
            return u.GetDlgItem(hwnd, control_id)

        def text(control_id):
            buffer = c.create_unicode_buffer(32768)
            send(control(control_id), 0xD, len(buffer), c.addressof(buffer))
            return buffer.value

        def expect(condition, name):
            if not condition:
                raise AssertionError(name)
            checks.append(name)

        def click(control_id):
            send(control(control_id), 0xF5)

        def select(path):
            u.PostMessageW(hwnd, 0x111, PICKER, control(PICKER))
            picker = wait_for(lambda: find_window(process.pid, lambda wnd: u.GetWindow(wnd, 4) == hwnd),
                              "Native file dialog missing")
            if path is None:
                u.PostMessageW(picker, 0x111, 2, 0)
            else:
                filename = u.GetDlgItem(picker, 0x480)
                if not filename:
                    combo = u.GetDlgItem(picker, 0x47C)
                    filename = (u.FindWindowExW(combo, None, "Edit", None)
                                or u.FindWindowExW(u.FindWindowExW(combo, None, "ComboBox", None), None, "Edit", None)
                                if combo else None)
                if not filename:
                    raise RuntimeError("Native filename edit missing")
                filename_text = c.create_unicode_buffer(str(path))
                send(filename, 0xC, 0, c.addressof(filename_text))
                u.PostMessageW(picker, 0x111, 1, u.GetDlgItem(picker, 1))
            wait_for(lambda: not u.IsWindowVisible(picker), "File dialog did not close")
            if path is not None:
                wait_for(lambda: text(FILENAME) == str(path), "Selected path mismatch")

        def count():
            return send(control(HISTORY), 0x1004)

        def focus():
            info = GuiThreadInfo(size=c.sizeof(GuiThreadInfo))
            u.GetGUIThreadInfo(u.GetWindowThreadProcessId(hwnd, None), c.byref(info))
            return info.focus

        def shot(name):
            capture(hwnd, out / f"{name}.png")

        def resize(width, height):
            width, height = (round(value * dpi / 96) for value in (width, height))
            u.SetWindowPos(hwnd, None, 0, 0, width, height, 0x4000 | 0x2 | 0x4)
            def reached():
                bounds = w.RECT()
                u.GetWindowRect(hwnd, c.byref(bounds))
                return (bounds.right - bounds.left, bounds.bottom - bounds.top) == (width, height)
            wait_for(reached, "Window resize did not complete")

        def inspect(path, history_count, screenshot=None):
            select(path)
            expect(text(STATUS) == "준비" and text(VERDICT) == "판정 대기", "File selection clears the previous verdict")
            wait_for(lambda: u.IsWindowEnabled(control(START)), "Worker slot was not released")
            click(START)
            wait_for(lambda: text(STATUS) == "완료", f"File did not complete: {path}")
            row = expected[path]
            expect(text(VERDICT) == {"PASS": "정상", "FAIL": "불량"}[row["decision"]]
                   and text(METADATA) == f"{row['width']} × {row['height']} px"
                   and text(DETAIL).startswith(f"후보 점수 {float(row['score']):.4f}\n")
                   and count() == history_count, f"CLI equivalence for {row['decision']}")
            if screenshot:
                shot(screenshot)

        dpi = u.GetDpiForWindow(hwnd)
        expect(text(STATUS) == "대기" and not u.IsWindowEnabled(control(START)), "Empty input blocks start")
        select(passed)
        u.PostMessageW(control(PICKER), 0x100, 9, 0)
        wait_for(lambda: focus() == control(START), "Tab did not move from file selection to Start")
        checks.append("Tab moves to the enabled Start button")
        inspect(passed, 1, "01-pass")
        select(None)
        expect(text(FILENAME) == str(passed) and text(VERDICT) == "정상", "File dialog cancel preserves current result")
        inspect(failed, 2, "02-fail")
        bounds, viewer = w.RECT(), w.RECT()
        u.GetWindowRect(hwnd, c.byref(bounds))
        u.GetWindowRect(control(IMAGE), c.byref(viewer))
        box = (viewer.left - bounds.left, viewer.top - bounds.top, viewer.right - bounds.left, viewer.bottom - bounds.top)
        click(CANDIDATE)
        shot("03-original")
        cli_images = out / "cli-1/images/000001"
        for screenshot, cli_image in (("02-fail", "overlay"), ("03-original", "original")):
            check_displayed_image(out / f"{screenshot}.png", box, cli_images / f"{cli_image}.png")
            checks.append(f"Viewer matches CLI {cli_image} pixels")

        for path, message, label in (
            (corrupt, "PNG 손상\n다른 이미지 선택", "Bad IEND CRC"),
            (missing, "파일 접근 실패\n경로와 권한 확인", "File removed after selection"),
            (oversized, "크기 한도 초과\n16 MiB, 1,600만 픽셀", "PNG above the pixel limit"),
            (unsupported, "지원하지 않는 형식\n8비트 컬러 PNG 선택", "Unsupported grayscale PNG"),
        ):
            select(path)
            if path == missing:
                path.unlink()
            click(START)
            wait_for(lambda: text(STATUS) == "처리 오류", f"{label} did not show an error")
            expect(text(VERDICT) == "판정 대기" and text(DETAIL) == message and count() == 2,
                   f"{label} shows recovery text and preserves history")
            if path == corrupt:
                shot("04-error")
        inspect(passed, 3)
        select(large)
        click(START)
        expect(text(STATUS) == "검사 중", "Large PNG entered background inspection")
        resize(800, 520)
        expect(text(STATUS) == "검사 중", "Window resized and responded during inspection")
        click(STOP)
        expect(text(STATUS) == "중지" and text(VERDICT) == "판정 대기", "Stop invalidates the current verdict")
        shot("05-cancelled")
        inspect(failed, 4)
        expect(count() == 4, "Stopped inspection never entered history")
        resize(760, 420)
        send(hwnd, 0x115, 7)
        def history_visible():
            window, listing = w.RECT(), w.RECT()
            u.GetWindowRect(hwnd, c.byref(window))
            u.GetWindowRect(control(HISTORY), c.byref(listing))
            return u.GetScrollPos(hwnd, 1) > 0 and window.top < listing.top < window.bottom
        wait_for(history_visible, "History did not scroll into the small window")
        expect(text(FILENAME) == str(failed), "Small window scroll and full path")
        time.sleep(0.1)
        shot("06-small")
        resize(960, 680)
        select(large)
        click(START)
        expect(text(STATUS) == "검사 중", "Close exercised during active inspection")
        u.PostMessageW(hwnd, 0x10, 0, 0)
        exit_code = process.wait(timeout=8)
        expect(exit_code == 0, f"Close drains the worker and exits: exit code {exit_code:#x}")
        data = {"dpi": dpi, "checks": checks, "exit_code": exit_code,
                "inputs": {row["decision"]: {
            key: row[key] for key in ("image_sha256", "width", "height", "score", "decision")
        } for row in expected.values()}}
        (out / "verification.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        print(f"S09 GUI checks passed ({len(checks)}), DPI={dpi}. Evidence: {out}")
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=8)


if __name__ == "__main__":
    main()
