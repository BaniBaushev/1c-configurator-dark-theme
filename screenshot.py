# -*- coding: utf-8 -*-
"""Screenshot the main window of 1cv8.exe via GDI BitBlt (pure ctypes)."""
import ctypes, sys, subprocess
from ctypes import wintypes

u = ctypes.windll.user32
g = ctypes.windll.gdi32
k = ctypes.windll.kernel32

def find_pid(name="1cv8.exe"):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"]).decode("cp866", errors="replace")
    for line in out.strip().splitlines():
        if name.lower() in line.lower():
            return int(line.strip().strip('"').split('","')[1])
    return None

pid = find_pid()
out_path = sys.argv[1]

target = {"hwnd": None, "area": 0}
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
def cb(hwnd, lp):
    p = wintypes.DWORD()
    u.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
    if p.value == pid and u.IsWindowVisible(hwnd):
        txt = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(hwnd, txt, 256)
        if "Конфигуратор" in txt.value:
            rc2 = wintypes.RECT()
            u.GetWindowRect(hwnd, ctypes.byref(rc2))
            area = (rc2.right - rc2.left) * (rc2.bottom - rc2.top)
            if area > target["area"]:
                target["area"] = area
                target["hwnd"] = hwnd
    return True
u.EnumWindows(WNDENUMPROC(cb), 0)
hwnd = target["hwnd"]
if not hwnd:
    print("[!] window not found"); sys.exit(1)

rc = wintypes.RECT()
u.GetWindowRect(hwnd, ctypes.byref(rc))
w, h = rc.right - rc.left, rc.bottom - rc.top
print(f"[i] hwnd=0x{hwnd:X} {w}x{h}")

hdcWin = u.GetWindowDC(hwnd)
hdcMem = g.CreateCompatibleDC(hdcWin)
hbm = g.CreateCompatibleBitmap(hdcWin, w, h)
g.SelectObject(hdcMem, hbm)
u.PrintWindow(hwnd, hdcMem, 2)  # PW_RENDERFULLCONTENT

# Save BMP -> convert with PIL
class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]

bih = BITMAPINFOHEADER()
bih.biSize = ctypes.sizeof(BITMAPINFOHEADER)
bih.biWidth = w
bih.biHeight = -h  # top-down
bih.biPlanes = 1
bih.biBitCount = 32
bih.biCompression = 0
buf = ctypes.create_string_buffer(w * h * 4)
g.GetDIBits(hdcMem, hbm, 0, h, buf, ctypes.byref(bih), 0)

g.DeleteObject(hbm)
g.DeleteDC(hdcMem)
u.ReleaseDC(hwnd, hdcWin)

from PIL import Image
img = Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1)
img.convert("RGB").save(out_path)
print(f"[+] saved {out_path}")
