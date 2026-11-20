"""Exercise the actual S03 Windows app and capture only its window."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import subprocess
import time
from PIL import Image

u = c.WinDLL("user32", use_last_error=True)
g = c.WinDLL("gdi32", use_last_error=True)
u.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SendMessageW.restype = c.c_ssize_t
u.GetDlgItem.argtypes = [w.HWND, c.c_int]
u.GetDlgItem.restype = w.HWND
u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.IsWindowEnabled.argtypes = [w.HWND]
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetDpiForWindow.argtypes = [w.HWND]
u.GetWindowLongW.argtypes = [w.HWND, c.c_int]
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

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=Path("artifacts/S03/ARM64/Release/WeldScope.App.exe"))
    parser.add_argument("--output", type=Path, default=Path("artifacts/S03/ui-verification"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    u.SetProcessDpiAwarenessContext(c.c_void_p(-4))
    process = subprocess.Popen([str(args.exe.resolve())])
    hwnd = None
    checks = []
    screenshots = []
    callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def find(window, _):
        nonlocal hwnd
        pid = w.DWORD()
        u.GetWindowThreadProcessId(window, c.byref(pid))
        if pid.value == process.pid and u.IsWindowVisible(window): hwnd = window
        return True
    callback = callback_type(find)
    try:
        for _ in range(100):
            u.EnumWindows(callback, 0)
            if hwnd and u.GetDlgItem(hwnd, 1104): break
            if process.poll() is not None: raise RuntimeError(f"App exited: {process.returncode}")
            time.sleep(0.1)
        if not hwnd: raise RuntimeError("App window did not appear")
        def control(control_id): return u.GetDlgItem(hwnd, control_id)
        def text(control_id):
            buffer = c.create_unicode_buffer(1024)
            u.SendMessageW(control(control_id), 0xD, len(buffer), c.addressof(buffer))
            return buffer.value
        def expect(condition, name):
            if not condition: raise AssertionError(name)
            checks.append(name)
        def click(control_id):
            u.SendMessageW(control(control_id), 0xF5, 0, 0)
            time.sleep(0.08)
        def select(index):
            u.SendMessageW(control(1100), 0x14E, index, 0)
            u.SendMessageW(hwnd, 0x111, 1100 | (1 << 16), control(1100))
            time.sleep(0.12)
        def shot(name):
            time.sleep(0.15)
            capture(hwnd, args.output / f"{name}.png")
            screenshots.append(f"{name}.png")
        def count(): return u.SendMessageW(control(1104), 0x1004, 0, 0)

        expect(u.GetWindowLongW(hwnd, -16) & 0xC00000 == 0xC00000, "window has a native title bar and system buttons")
        expect(text(1009) == "대기" and not u.IsWindowEnabled(control(1101)), "empty input blocks start")
        shot("01-empty")
        for index, verdict in enumerate(["정상", "불량", "검토 필요"]):
            select(index)
            expect(text(1007) == "판정 대기" and text(1009) == "준비", "selection clears previous verdict")
            click(1101)
            expect(text(1009) == "검사 중" and not u.IsWindowEnabled(control(1100)) and u.IsWindowEnabled(control(1102)), "running locks input and allows stop")
            if index == 0: shot("02-running")
            time.sleep(1.3)
            expect(text(1007) == verdict and text(1009) == "완료", f"completed verdict: {verdict}")
            expect(count() == index + 1, "completed sample adds one history row")
            shot(["03-normal", "04-defect", "06-review"][index])
            if index == 1:
                click(1103)
                expect(u.SendMessageW(control(1103), 0xF0, 0, 0) == 0, "candidate checkbox toggles off")
                shot("05-candidate-off")
                click(1103)
        select(1)
        click(1101)
        click(1102)
        time.sleep(1.4)
        expect(text(1009) == "중지" and text(1007) == "판정 대기" and count() == 3, "cancelled timer produces no verdict or history")
        shot("07-cancelled")
        select(3)
        expect(text(1009) == "처리 오류" and not u.IsWindowEnabled(control(1101)), "corrupt image is blocked and clears verdict")
        shot("08-error")
        select(4)
        expect(text(1009) == "준비" and "긴_파일명" in text(1005), "long filename and recovery after decode failure")
        expect("train/OK/image_1.png" in text(1005), "long display name preserves the actual source path")
        click(1101)
        time.sleep(1.4)
        expect(text(1007) == "정상" and count() == 4, "valid input completes after error")
        shot("09-long-name")
        dpi = u.GetDpiForWindow(hwnd)
        def dip(value): return round(value * dpi / 96)
        u.SetWindowPos(hwnd, None, 0, 0, dip(960), dip(680), 0x6)
        time.sleep(0.2)
        shot("10-minimum-size")
        bounds = w.RECT()
        u.GetWindowRect(control(1104), c.byref(bounds))
        expect(bounds.bottom > bounds.top and bounds.right > bounds.left, "history has usable dimensions at 960 by 680")
        u.SetWindowPos(hwnd, None, 0, 0, dip(800), dip(520), 0x6)
        time.sleep(0.2)
        detail_bounds, candidate_bounds = w.RECT(), w.RECT()
        u.GetWindowRect(control(1010), c.byref(detail_bounds))
        u.GetWindowRect(control(1103), c.byref(candidate_bounds))
        expect(detail_bounds.bottom <= candidate_bounds.top, "small window keeps detail text separate from candidate checkbox")
        shot("11-small-window")
        u.SendMessageW(hwnd, 0x115, 7, 0)  # WM_VSCROLL / SB_BOTTOM
        time.sleep(0.2)
        footer_bounds, client_bounds = w.RECT(), w.RECT()
        u.GetWindowRect(control(1013), c.byref(footer_bounds))
        u.GetClientRect(hwnd, c.byref(client_bounds))
        origin = w.POINT(0, 0)
        u.ClientToScreen(hwnd, c.byref(origin))
        expect(footer_bounds.bottom <= origin.y + client_bounds.bottom, "small window scroll reaches footer and history")
        shot("12-small-window-scrolled")
        u.SendMessageW(hwnd, 0x115, 6, 0)  # SB_TOP
        # Send real keyboard events to this app only after confirming it owns the foreground.
        u.SetForegroundWindow(hwnd)
        u.GetForegroundWindow.restype = w.HWND
        if u.GetForegroundWindow() == hwnd:
            u.SendMessageW(hwnd, 0x28, control(1100), 1)  # WM_NEXTDLGCTL: explicit focus target
            u.keybd_event(9, 0, 0, 0)
            u.keybd_event(9, 0, 2, 0)
            time.sleep(0.15)
            class GuiThreadInfo(c.Structure):
                _fields_ = [("cbSize", w.DWORD), ("flags", w.DWORD)] + [(name, w.HWND) for name in ("active", "focus", "capture", "menu", "move", "caret")] + [("rect", w.RECT)]
            info = GuiThreadInfo()
            info.cbSize = c.sizeof(info)
            u.GetGUIThreadInfo(u.GetWindowThreadProcessId(hwnd, None), c.byref(info))
            expect(info.focus == control(1101), "Tab moves from sample picker to start button")
            u.keybd_event(9, 0, 0, 0)
            u.keybd_event(9, 0, 2, 0)
            time.sleep(0.15)
            u.GetGUIThreadInfo(u.GetWindowThreadProcessId(hwnd, None), c.byref(info))
            expect(info.focus == control(1104) and u.GetScrollPos(hwnd, 1) > 0, "Tab to history scrolls it into view in a small window")
        else:
            checks.append("keyboard test skipped: foreground activation denied")
        u.SetWindowPos(hwnd, None, 0, 0, dip(760), dip(420), 0x6)
        u.SendMessageW(hwnd, 0x115, 7, 0)
        shot("13-minimum-size-scrolled")
        report = {"checks": checks, "screenshots": screenshots, "window_dpi": dpi,
                  "exe": str(args.exe), "platform": "Windows ARM64", "scope": "Real MFC UI with predefined sample verdicts; no inspection accuracy claim"}
        (args.output / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, ensure_ascii=False, indent=2))
    finally:
        if hwnd: u.PostMessageW(hwnd, 0x10, 0, 0)
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.terminate()

if __name__ == "__main__": main()
