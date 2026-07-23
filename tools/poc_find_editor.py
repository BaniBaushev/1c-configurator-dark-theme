# -*- coding: utf-8 -*-
"""
PoC: поиск окна редактора модулей в процессе 1cv8.exe и определение его класса.
Цель: проверить, является ли редактор модулей контролом Scintilla (go/no-go для темизации
через SCI_STYLESET*) или кастомным owner-draw контролом.

Запуск: python poc_find_editor.py
Требование: запущенный конфигуратор 1С (желательно с открытым модулем).
Чистый ctypes, без сторонних зависимостей.
"""

import ctypes
from ctypes import wintypes
import json
import sys
from collections import defaultdict

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# --- WinAPI prototypes -------------------------------------------------------
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.EnumChildWindows.argtypes = [wintypes.HWND, WNDENUMPROC, wintypes.LPARAM]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]

TH32CS_SNAPPROCESS = 0x00000002

class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


def find_1c_processes():
    """Вернуть список PID процессов 1cv8*.exe."""
    pids = []
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == -1:
        return pids
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower().startswith("1cv8"):
            pids.append((entry.th32ProcessID, entry.szExeFile))
        ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    kernel32.CloseHandle(snap)
    return pids


def get_class_name(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def get_window_text(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(hwnd, buf, 512)
    return buf.value


def get_rect(hwnd):
    r = wintypes.RECT()
    if user32.GetWindowRect(hwnd, ctypes.byref(r)):
        return (r.left, r.top, r.right - r.left, r.bottom - r.top)
    return None


# Собираем все окна (топ-левел и дочерние), принадлежащие заданным PID
results = []  # (pid, hwnd, parent_hwnd, class, text, rect, visible)


def enum_top_cb(hwnd, lparam):
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    if pid.value in target_pids:
        results.append((pid.value, hwnd, 0, get_class_name(hwnd),
                        get_window_text(hwnd), get_rect(hwnd),
                        bool(user32.IsWindowVisible(hwnd))))
        # дочерние окна рекурсивно
        user32.EnumChildWindows(hwnd, WNDENUMPROC(enum_child_cb), 0)
    return True


def enum_child_cb(hwnd, lparam):
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    parent = user32.GetAncestor(hwnd, 2)  # GA_ROOT
    results.append((pid.value, hwnd, parent, get_class_name(hwnd),
                    get_window_text(hwnd), get_rect(hwnd),
                    bool(user32.IsWindowVisible(hwnd))))
    # рекурсия в глубину
    user32.EnumChildWindows(hwnd, WNDENUMPROC(enum_child_cb), 0)
    return True


user32.GetAncestor = user32.GetAncestor
user32.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]


def classify(class_name):
    """Эвристика: что это за контрол."""
    c = class_name.lower()
    if "scintilla" in c:
        return "SCINTILLA"
    if c.startswith(("edit", "richedit")):
        return "STD_EDIT"
    if "syslistview" in c:
        return "LISTVIEW"
    if "systreeview" in c:
        return "TREEVIEW"
    if "v8" in c or "1c" in c or "frame8" in c or "wndframe" in c:
        return "1C_CUSTOM"
    return "OTHER"


def main():
    global target_pids
    procs = find_1c_processes()
    if not procs:
        print("[!] Процессы 1cv8*.exe не найдены. Запустите конфигуратор 1С "
              "и откройте любой модуль, затем перезапустите скрипт.")
        sys.exit(1)

    target_pids = {pid for pid, _ in procs}
    print(f"[*] Найдены процессы 1С: {procs}")

    user32.EnumWindows(WNDENUMPROC(enum_top_cb), 0)
    print(f"[*] Всего окон/контролов в процессах 1С: {len(results)}\n")

    # Сводка по классам
    by_class = defaultdict(list)
    for row in results:
        by_class[row[3]].append(row)

    print("=== Сводка по классам окон ===")
    for cls, rows in sorted(by_class.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(rows):4d}  [{classify(cls):10s}]  {cls}")

    # Кандидаты в редактор модулей:
    # - Scintilla (лучший случай)
    # - крупные видимые кастомные контролы (вероятный owner-draw редактор)
    print("\n=== Кандидаты: Scintilla ===")
    scintilla = [r for r in results if "scintilla" in r[3].lower()]
    if scintilla:
        for r in scintilla:
            print(f"  HWND={r[1]:#x} class={r[3]} text={r[4]!r} rect={r[5]}")
    else:
        print("  не найдено")

    print("\n=== Кандидаты: крупные кастомные контролы (видимые, >= 300x200) ===")
    candidates = [
        r for r in results
        if r[6] and r[5] and r[5][2] >= 300 and r[5][3] >= 200
        and classify(r[3]) in ("1C_CUSTOM", "OTHER")
    ]
    for r in sorted(candidates, key=lambda x: -(x[5][2] * x[5][3]))[:15]:
        print(f"  HWND={r[1]:#010x} class={r[3]!r:40s} rect={r[5]} text={r[4][:60]!r}")

    # Сохраняем полный дамп для анализа
    dump = [
        {"pid": r[0], "hwnd": r[1], "class": r[3], "text": r[4],
         "rect": r[5], "visible": r[6]}
        for r in results
    ]
    out = "poc_window_dump.json"
    with open(out, "w", encoding="utf-8") as f:
        json.dump(dump, f, ensure_ascii=False, indent=2)
    print(f"\n[*] Полный дамп окон сохранён: {out}")

    print("\n=== Вердикт ===")
    if scintilla:
        print("[+] Редактор основан на Scintilla -> темизация через SCI_STYLESET* реальна.")
    else:
        print("[-] Scintilla не обнаружен. Редактор, скорее всего, кастомный owner-draw.")
        print("    Смотрите крупные кандидаты выше — их классы намекнут на природу контрола.")
        print("    Дальнейший шаг: Spy++/x64dbg на HWND кандидата, анализ WM_PAINT/WM_ERASEBKGND.")


if __name__ == "__main__":
    main()
