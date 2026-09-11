"""Targeted Windows game captures/hotkeys. Never captures a different process.

python tools/game_capture.py --pid 6072 --key toggle
python tools/game_capture.py --pid 6072 --shot screenshot.png
Keys: toggle=Ctrl+End, reload=Ctrl+Home, capture=Ctrl+PageDown, escape.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import time

from PIL import ImageGrab

u = c.WinDLL("user32", use_last_error=True)
u.GetForegroundWindow.restype = w.HWND
u.SetForegroundWindow.argtypes = [w.HWND]
u.IsIconic.argtypes = [w.HWND]
u.ShowWindow.argtypes = [w.HWND, c.c_int]
u.IsWindowVisible.argtypes = [w.HWND]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
u.SetProcessDpiAwarenessContext.argtypes = [c.c_void_p]
u.SetProcessDpiAwarenessContext(c.c_void_p(-4))


def owner(hwnd):
    pid = w.DWORD()
    u.GetWindowThreadProcessId(hwnd, c.byref(pid))
    return pid.value


class Mouse(c.Structure):
    _fields_ = [("dx", w.LONG), ("dy", w.LONG), ("data", w.DWORD),
                ("flags", w.DWORD), ("time", w.DWORD), ("extra", c.c_size_t)]


class Keyboard(c.Structure):
    _fields_ = [("vk", w.WORD), ("scan", w.WORD), ("flags", w.DWORD),
                ("time", w.DWORD), ("extra", c.c_size_t)]


class Payload(c.Union):
    _fields_ = [("mouse", Mouse), ("keyboard", Keyboard)]


class Input(c.Structure):
    _fields_ = [("type", w.DWORD), ("payload", Payload)]


u.SendInput.argtypes = [w.UINT, c.POINTER(Input), c.c_int]
u.SendInput.restype = w.UINT


def key(vk, release=False):
    # DirectInput games often ignore VK-only injection. Use scan codes, including
    # the extended-key bit for the navigation block used by the add-on hotkeys.
    flags = 8 | (2 if release else 0) | (1 if vk in (0x22, 0x23, 0x24) else 0)
    scan = u.MapVirtualKeyW(vk, 0)
    item = Input(1, Payload(keyboard=Keyboard(0, scan, flags, 0, 0)))
    if u.SendInput(1, c.byref(item), c.sizeof(Input)) != 1:
        raise OSError(c.get_last_error(), "SendInput failed")


def run(pid, shortcut, shot):
    windows = []
    callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)

    @callback_type
    def found(hwnd, _):
        if owner(hwnd) == pid and u.IsWindowVisible(hwnd):
            rect = w.RECT()
            u.GetClientRect(hwnd, c.byref(rect))
            windows.append((rect.right * rect.bottom, hwnd))
        return True

    u.EnumWindows(found, 0)
    if not windows:
        raise RuntimeError(f"No visible window for process {pid}")
    hwnd = max(windows)[1]
    if u.IsIconic(hwnd):
        u.ShowWindow(hwnd, 9)
    u.SetForegroundWindow(hwnd)
    time.sleep(1)
    if owner(u.GetForegroundWindow()) != pid:
        raise RuntimeError("Game is not foreground; no keys or screenshot sent")
    if shortcut:
        keys = {"toggle": [0x11, 0x23], "reload": [0x11, 0x24],
                "capture": [0x11, 0x22], "escape": [0x1B], "enter": [0x0D]}[shortcut]
        pressed = []
        try:
            for vk in keys:
                key(vk)
                pressed.append(vk)
            time.sleep(0.4)
        finally:
            for vk in reversed(pressed):
                key(vk, True)
        print("key", shortcut, "sent to process", pid)
        time.sleep(1)
    if shot:
        if owner(u.GetForegroundWindow()) != pid:
            raise RuntimeError("Foreground changed; screenshot refused")
        rect, origin = w.RECT(), w.POINT(0, 0)
        if not u.GetClientRect(hwnd, c.byref(rect)) or not u.ClientToScreen(hwnd, c.byref(origin)):
            raise OSError("Cannot locate game client area")
        box = (origin.x, origin.y, origin.x + rect.right, origin.y + rect.bottom)
        image = ImageGrab.grab(bbox=box, all_screens=True)
        if owner(u.GetForegroundWindow()) != pid:
            raise RuntimeError("Foreground changed during capture; screenshot refused")
        shot.parent.mkdir(parents=True, exist_ok=True)
        image.save(shot)
        print("game client", box, "saved to", shot)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--key", choices=["toggle", "reload", "capture", "escape", "enter"])
    parser.add_argument("--shot", type=Path)
    args = parser.parse_args()
    run(args.pid, args.key, args.shot)
