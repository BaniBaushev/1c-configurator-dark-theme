# -*- coding: utf-8 -*-
# Right-click in the code editor of 1cv8.exe, then capture the popup menu
# window (class #32768) via PrintWindow.
import ctypes, time, sys
from ctypes import wintypes as W
from PIL import Image

u = ctypes.windll.user32
g = ctypes.windll.gdi32

def find_main():
    res = []
    @ctypes.WINFUNCTYPE(ctypes.c_bool, W.HWND, W.LPARAM)
    def cb(hwnd, lp):
        if not u.IsWindowVisible(hwnd):
            return True
        pid = W.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value != 975792:
            return True
        buf = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(hwnd, buf, 256)
        rc = W.RECT(); u.GetWindowRect(hwnd, ctypes.byref(rc))
        area = (rc.right - rc.left) * (rc.bottom - rc.top)
        if buf.value and u"Конфигуратор" in buf.value:
            res.append((area, hwnd, buf.value))
        return True
    u.EnumWindows(cb, 0)
    res.sort(reverse=True)
    return res[0][1] if res else None

hwnd = find_main()
print("[i] main hwnd:", hex(hwnd))
u.ShowWindow(hwnd, 9)
u.SetForegroundWindow(hwnd)
u.ShowWindow(hwnd, 3)
time.sleep(1.0)
rc = W.RECT(); u.GetWindowRect(hwnd, ctypes.byref(rc))
print("[i] rect:", rc.left, rc.top, rc.right, rc.bottom)
# click into code editor area (left-center of window), then right-click
cx = rc.left + int((rc.right - rc.left) * 0.25)
cy = rc.top + int((rc.bottom - rc.top) * 0.55)
# message-based clicks (session may block foreground/SendInput)
lx, ly = cx - rc.left, cy - rc.top
lp = (ly << 16) | (lx & 0xFFFF)
child = u.ChildWindowFromPoint(hwnd, W.POINT(lx, ly))
target = child or hwnd
print("[i] target child:", hex(target) if target else None)
# convert to client coords of target
pt = W.POINT(cx, cy)
u.ScreenToClient(target, ctypes.byref(pt))
lp2 = (pt.y << 16) | (pt.x & 0xFFFF)
u.PostMessageW(target, 0x0201, 1, lp2); time.sleep(0.1)   # WM_LBUTTONDOWN (focus)
u.PostMessageW(target, 0x0202, 0, lp2); time.sleep(0.3)   # WM_LBUTTONUP
u.PostMessageW(target, 0x0204, 1, lp2); time.sleep(0.1)   # WM_RBUTTONDOWN
u.PostMessageW(target, 0x0205, 0, lp2); time.sleep(0.3)   # WM_RBUTTONUP
u.PostMessageW(target, 0x007B, hwnd, lp2)                 # WM_CONTEXTMENU
time.sleep(1.2)

# find popup menu window (#32768)
menus = []
@ctypes.WINFUNCTYPE(ctypes.c_bool, W.HWND, W.LPARAM)
def cb2(h, lp):
    buf = ctypes.create_unicode_buffer(64)
    u.GetClassNameW(h, buf, 64)
    if buf.value == "#32768" and u.IsWindowVisible(h):
        menus.append(h)
    return True
u.EnumWindows(cb2, 0)
print("[i] menu windows:", [hex(m) for m in menus])
if not menus:
    print("[!] no popup menu found")
    sys.exit(1)
m = menus[0]
mrc = W.RECT(); u.GetWindowRect(m, ctypes.byref(mrc))
w, h = mrc.right - mrc.left, mrc.bottom - mrc.top
print("[i] menu rect:", w, h)
hdcW = u.GetWindowDC(m)
hdcM = g.CreateCompatibleDC(hdcW)
bmp = g.CreateCompatibleBitmap(hdcW, w, h)
g.SelectObject(hdcM, bmp)
ok = u.PrintWindow(m, hdcM, 2)  # PW_RENDERFULLCONTENT
print("[i] PrintWindow:", ok)
class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", W.DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
                ("biPlanes", W.WORD), ("biBitCount", W.WORD), ("biCompression", W.DWORD),
                ("biSizeImage", W.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", W.DWORD), ("biClrImportant", W.DWORD)]
bmi = BITMAPINFOHEADER(); bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
bmi.biWidth = w; bmi.biHeight = -h; bmi.biPlanes = 1; bmi.biBitCount = 32
buf = ctypes.create_string_buffer(w * h * 4)
g.GetDIBits(hdcM, bmp, 0, h, buf, ctypes.byref(bmi), 0)
img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1)
out = sys.argv[1] if len(sys.argv) > 1 else "after17.png"
img.save(out)
print("[+] saved", out)
g.DeleteObject(bmp); g.DeleteDC(hdcM); u.ReleaseDC(m, hdcW)
# close menu
u.keybd_event(0x1B, 0, 0, 0); u.keybd_event(0x1B, 0, 2, 0)
