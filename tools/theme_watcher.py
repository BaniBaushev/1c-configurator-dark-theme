#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Theme Watcher: автоприменение тёмной темы к каждому 1cv8.exe.

Следит за появлением процессов 1cv8.exe и инжектирует ThemeHook3
(через тот же механизм, что и inject.py: CreateRemoteThread -> LoadLibraryW).

- Инжектирует один раз в каждый новый процесс (журнал pid).
- Повторная инжекция безопасна: LoadLibrary для уже загруженной DLL
  просто вернёт тот же hModule (DllMain повторно не выполняется).
- Лог: logs/watcher_log.txt
- Остановка: Ctrl+C.

Автозапуск: tools/watcher_install.bat (HKCU Run, без админа).
Откат:      tools/watcher_uninstall.bat
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DLL = os.path.join(ROOT, "builds", "ThemeHook3_v55.dll")
LOG = os.path.join(ROOT, "logs", "watcher_log.txt")
POLL_SEC = 2.0
# КРИТИЧНО: инжектировать рано (как было при SETTLE_SEC=4) НЕЛЬЗЯ.
# При ранней инжекции хуки перекрашивают текст в момент, когда 1С строит
# стартовые кэши отрисовки — перекрашенные прогоны (emboss-пары) запекаются
# в кэш и отравляют процесс навсегда (двоение переживает даже выгрузку DLL).
# Поэтому ждём появления видимого главного окна "Конфигуратор" и только потом
# выдерживаем паузу на достройку кэшей.
WINDOW_TIMEOUT_SEC = 300.0  # сколько ждём появления окна, прежде чем инжектить вслепую
SETTLE_SEC = 25.0           # пауза ПОСЛЕ появления окна "Конфигуратор"

k = ctypes.windll.kernel32
k.GetProcAddress.restype = ctypes.c_void_p
k.GetProcAddress.argtypes = [wintypes.HMODULE, wintypes.LPCSTR]
k.GetModuleHandleW.restype = wintypes.HMODULE
k.VirtualAllocEx.restype = ctypes.c_void_p
k.OpenProcess.restype = ctypes.wintypes.HANDLE if hasattr(ctypes.wintypes, 'HANDLE') else ctypes.c_void_p
k.OpenProcess.restype = ctypes.c_void_p
k.CreateRemoteThread.restype = ctypes.c_void_p

PROCESS_ALL_ACCESS_NEEDED = 0x1F0FFF
MEM_COMMIT_RESERVE = 0x3000
PAGE_READWRITE = 0x04


def log(msg):
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    print(line, flush=True)
    try:
        os.makedirs(os.path.dirname(LOG), exist_ok=True)
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def list_pids(name="1cv8.exe"):
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"]
        ).decode("cp866", errors="replace")
    except subprocess.CalledProcessError:
        return []
    pids = []
    for line in out.strip().splitlines():
        if name.lower() in line.lower():
            try:
                pids.append(int(line.strip().strip('"').split('","')[1]))
            except (IndexError, ValueError):
                pass
    return pids


def find_main_window(pid, needle="Конфигуратор"):
    """Видимое top-level окно процесса с needle в заголовке. 0, если нет."""
    u = ctypes.windll.user32
    found = {"hwnd": 0}
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, lp):
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and u.IsWindowVisible(hwnd):
            txt = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(hwnd, txt, 256)
            if needle in txt.value:
                found["hwnd"] = hwnd
                return False
        return True
    u.EnumWindows(WNDENUMPROC(cb), 0)
    return found["hwnd"]


def remote_loadlib_addr(h, wow, pid):
    """Адрес LoadLibraryW в целевом процессе (учёт wow64). Логика из inject.py."""
    if not wow:
        return k.GetProcAddress(k.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW")
    import struct
    k32_path = os.path.join(os.environ["SystemRoot"], "SysWOW64", "kernel32.dll")
    with open(k32_path, "rb") as f:
        data = f.read()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    n_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec_off = pe_off + 24 + opt_size
    sections = []
    for i in range(n_sec):
        vs, va, rawsz, rawoff = struct.unpack_from("<IIII", data, sec_off + i * 40 + 8)
        sections.append((va, max(vs, rawsz), rawoff))
    def r2o(rva):
        for va, sz, rawoff in sections:
            if va <= rva < va + sz:
                return rawoff + (rva - va)
        return rva
    eo = r2o(struct.unpack_from("<I", data, pe_off + 0x78)[0])
    n_funcs, n_names = struct.unpack_from("<II", data, eo + 0x14)
    funcs_rva, names_rva, ords_rva = struct.unpack_from("<III", data, eo + 0x1C)
    rva = None
    for i in range(n_names):
        name_off = r2o(struct.unpack_from("<I", data, r2o(names_rva) + i * 4)[0])
        nm = data[name_off:data.index(b"\0", name_off)].decode()
        if nm == "LoadLibraryW":
            ord_i = struct.unpack_from("<H", data, r2o(ords_rva) + i * 2)[0]
            rva = struct.unpack_from("<I", data, r2o(funcs_rva) + ord_i * 4)[0]
            break
    assert rva, "LoadLibraryW RVA not found"

    TH32CS_SNAPMODULE = 0x8
    TH32CS_SNAPMODULE32 = 0x10
    class MODULEENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
                    ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
                    ("modBaseSize", wintypes.DWORD), ("hModule", wintypes.HMODULE),
                    ("szModule", wintypes.WCHAR * 256), ("szExePath", wintypes.WCHAR * 260)]
    snap = k.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32W(); me.dwSize = ctypes.sizeof(MODULEENTRY32W)
    base = None
    if k.Module32FirstW(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == "kernel32.dll":
                base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value
                break
            if not k.Module32NextW(snap, ctypes.byref(me)):
                break
    k.CloseHandle(snap)
    assert base, "kernel32.dll not found in remote process"
    return base + rva


def inject(pid):
    """Инжектирует DLL в процесс. Возвращает True при успехе."""
    h = k.OpenProcess(PROCESS_ALL_ACCESS_NEEDED, False, pid)
    if not h:
        log(f"pid={pid}: OpenProcess failed err={k.GetLastError()}")
        return False
    try:
        wow = wintypes.BOOL()
        k.IsWow64Process(h, ctypes.byref(wow))
        path_bytes = (DLL + "\0").encode("utf-16-le")
        addr = k.VirtualAllocEx(h, None, len(path_bytes), MEM_COMMIT_RESERVE, PAGE_READWRITE)
        if not addr:
            log(f"pid={pid}: VirtualAllocEx failed"); return False
        written = ctypes.c_size_t(0)
        if not k.WriteProcessMemory(h, addr, path_bytes, len(path_bytes), ctypes.byref(written)):
            log(f"pid={pid}: WriteProcessMemory failed"); return False
        loadlib = remote_loadlib_addr(h, bool(wow.value), pid)
        tid = wintypes.DWORD(0)
        th = k.CreateRemoteThread(h, None, 0, ctypes.c_void_p(loadlib), ctypes.c_void_p(addr), 0, ctypes.byref(tid))
        if not th:
            log(f"pid={pid}: CreateRemoteThread failed err={k.GetLastError()} (antivirus?)"); return False
        k.WaitForSingleObject(th, 15000)
        code = wintypes.DWORD(0)
        k.GetExitCodeThread(th, ctypes.byref(code))
        k.CloseHandle(th)
        k.VirtualFreeEx(h, addr, 0, 0x8000)
        if code.value == 0:
            log(f"pid={pid}: LoadLibraryW вернул NULL — DLL не загрузилась"); return False
        log(f"pid={pid}: тема применена (hModule=0x{code.value:08X})")
        return True
    except Exception as e:
        log(f"pid={pid}: исключение {e!r}")
        return False
    finally:
        k.CloseHandle(h)


def main():
    if not os.path.isfile(DLL):
        print(f"[!] DLL не найдена: {DLL}")
        return 2
    log(f"watcher стартовал, DLL={DLL}")
    seen_pending = {}   # pid -> время первого обнаружения процесса
    ready_since = {}    # pid -> время, когда появилось окно "Конфигуратор"
    injected = set()    # pid, куда уже заинжектили
    try:
        while True:
            pids = list_pids()
            now = time.time()
            for pid in pids:
                if pid in injected:
                    continue
                if pid not in seen_pending:
                    seen_pending[pid] = now
                    log(f"pid={pid}: обнаружен 1cv8.exe, жду окно 'Конфигуратор'...")
                    continue
                if pid not in ready_since:
                    if find_main_window(pid):
                        ready_since[pid] = now
                        log(f"pid={pid}: окно найдено, жду {SETTLE_SEC:.0f}с на достройку кэшей...")
                    elif now - seen_pending[pid] >= WINDOW_TIMEOUT_SEC:
                        log(f"pid={pid}: окно не появилось за {WINDOW_TIMEOUT_SEC:.0f}с, инжектирую вслепую")
                        ready_since[pid] = now - SETTLE_SEC  # инъекция на этом же цикле
                    continue
                if now - ready_since[pid] >= SETTLE_SEC:
                    if inject(pid):
                        injected.add(pid)
                    else:
                        injected.add(pid)  # не ддосим повторами; ручная инжекция всегда доступна
                    seen_pending.pop(pid, None)
                    ready_since.pop(pid, None)
            # чистка умерших pid
            dead = [p for p in list(seen_pending) + list(injected) if p not in pids]
            for p in dead:
                seen_pending.pop(p, None)
                ready_since.pop(p, None)
                injected.discard(p)
            time.sleep(POLL_SEC)
    except KeyboardInterrupt:
        log("watcher остановлен пользователем")
    return 0


if __name__ == "__main__":
    sys.exit(main())
