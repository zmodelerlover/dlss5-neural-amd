"""Show a seqgen sequence in an ordinary window, for a capture tool (Magpie) to process it live.

    python tools/seq_player.py corpus/pan-int [--fps 60] [--loops 3] [--hold 120]

The reference's temporal route only exists inside Magpie, which captures a window: this is that
window. Frames are drawn 1:1 at the sequence's own size, one per 1/fps, the first frame held
`hold` frames first so the capture has settled before the motion starts. Esc or closing the
window stops it. Nothing here tags a frame: the reference's dumps are matched back to the corpus by
content (tools/temporal_metrics.py reads them the same way as a framecheck run).
"""
import argparse
import ctypes
import time
from ctypes import wintypes
from pathlib import Path

user32, gdi32, kernel32 = ctypes.windll.user32, ctypes.windll.gdi32, ctypes.windll.kernel32
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
kernel32.GetModuleHandleW.restype = wintypes.HMODULE
user32.CreateWindowExW.restype = wintypes.HWND
user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU,
                                   wintypes.HINSTANCE, wintypes.LPVOID]
user32.GetDC.restype = wintypes.HDC
user32.GetDC.argtypes = [wintypes.HWND]
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.DestroyWindow.argtypes = [wintypes.HWND]
user32.SetForegroundWindow.argtypes = [wintypes.HWND]
user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, wintypes.LPVOID]
user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
gdi32.SetDIBitsToDevice.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD, wintypes.DWORD,
                                    ctypes.c_int, ctypes.c_int, wintypes.UINT, wintypes.UINT, ctypes.c_char_p,
                                    ctypes.c_void_p, wintypes.UINT]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wintypes.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", wintypes.HINSTANCE), ("hIcon", wintypes.HICON),
                ("hCursor", wintypes.HANDLE), ("hbrBackground", wintypes.HBRUSH),
                ("lpszMenuName", wintypes.LPCWSTR), ("lpszClassName", wintypes.LPCWSTR)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]


def load(path):
    """A P6 frame as bottom-up BGRX, which is what a 32-bit DIB wants."""
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    rgb = parts[3]
    rows = []
    for y in range(h - 1, -1, -1):
        row = rgb[y * w * 3:(y + 1) * w * 3]
        bgrx = bytearray(w * 4)
        bgrx[0::4], bgrx[1::4], bgrx[2::4] = row[2::3], row[1::3], row[0::3]
        rows.append(bytes(bgrx))
    return w, h, b"".join(rows)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("seq", type=Path)
    p.add_argument("--fps", type=float, default=60.0)
    p.add_argument("--loops", type=int, default=3)
    p.add_argument("--hold", type=int, default=120)
    a = p.parse_args()
    frames = [load(f) for f in sorted(a.seq.glob("frame*.ppm"))]
    w, h = frames[0][0], frames[0][1]

    stop = []

    def proc(hwnd, msg, wp, lp):
        if msg == 0x0010 or (msg == 0x0100 and wp == 0x1B):  # WM_CLOSE, Esc
            stop.append(1)
            return 0
        return user32.DefWindowProcW(hwnd, msg, wp, lp)

    wndproc = WNDPROC(proc)
    wc = WNDCLASSW(lpfnWndProc=wndproc, hInstance=kernel32.GetModuleHandleW(None), lpszClassName="seq_player")
    user32.RegisterClassW(ctypes.byref(wc))
    rect = wintypes.RECT(0, 0, w, h)
    style = 0x00C00000 | 0x00080000  # WS_CAPTION | WS_SYSMENU
    user32.AdjustWindowRect(ctypes.byref(rect), style, False)
    hwnd = user32.CreateWindowExW(0, "seq_player", f"seq-player {a.seq.name}", style | 0x10000000, 100, 100,
                                  rect.right - rect.left, rect.bottom - rect.top, None, None, wc.hInstance, None)
    # A Magpie auto-scale profile starts on the foreground window, and Windows refuses the foreground
    # to a process that does not own it: borrow the owner's input queue for the call.
    owner = user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), None)
    me = kernel32.GetCurrentThreadId()
    user32.AttachThreadInput(me, owner, True)
    user32.SetForegroundWindow(hwnd)
    user32.AttachThreadInput(me, owner, False)
    if user32.GetForegroundWindow() != hwnd:
        print("seq_player: could not take the foreground; an auto-scale profile will not start", flush=True)
    hdc = user32.GetDC(hwnd)
    bmi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, h, 1, 32, 0, 0, 0, 0, 0, 0)
    order = [0] * a.hold + [i for _ in range(a.loops) for i in range(len(frames))]
    period, start = 1.0 / a.fps, time.perf_counter()
    msg = wintypes.MSG()
    for n, i in enumerate(order):
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        if stop:
            break
        gdi32.SetDIBitsToDevice(hdc, 0, 0, w, h, 0, 0, 0, h, frames[i][2], ctypes.byref(bmi), 0)
        while time.perf_counter() < start + (n + 1) * period:
            time.sleep(0.0005)
    user32.ReleaseDC(hwnd, hdc)
    user32.DestroyWindow(hwnd)


if __name__ == "__main__":
    main()
