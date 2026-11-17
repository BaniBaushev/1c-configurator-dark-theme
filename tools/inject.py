#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Загрузка темы в уже открытый конфигуратор 1С. Чистый ctypes, без новых пакетов."""
from __future__ import annotations

import argparse
import ctypes
import os
import struct
import subprocess
import sys
from ctypes import wintypes

k = ctypes.windll.kernel32
k.OpenProcess.restype = ctypes.c_void_p
k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k.VirtualAllocEx.restype = ctypes.c_void_p
k.VirtualAllocEx.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD,
]
k.CreateRemoteThread.restype = ctypes.c_void_p
k.CreateRemoteThread.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
]
k.CloseHandle.argtypes = [wintypes.HANDLE]
k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
k.WaitForSingleObject.restype = wintypes.DWORD
k.IsWow64Process.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
k.IsWow64Process.restype = wintypes.BOOL
k.GetExitCodeThread.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
k.GetExitCodeThread.restype = wintypes.BOOL
k.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
k.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
k.WriteProcessMemory.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
k.WriteProcessMemory.restype = wintypes.BOOL
k.VirtualFreeEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD]
k.VirtualFreeEx.restype = wintypes.BOOL

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)
BUILDS_DIR = os.path.join(REPO_ROOT, "builds")

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_CREATE_THREAD = 0x0002
PROCESS_VM_OPERATION = 0x0008
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
INJECT_ACCESS = (
    PROCESS_QUERY_LIMITED_INFORMATION
    | PROCESS_CREATE_THREAD
    | PROCESS_VM_OPERATION
    | PROCESS_VM_READ
    | PROCESS_VM_WRITE
)
UNLOAD_ACCESS = (
    PROCESS_QUERY_LIMITED_INFORMATION
    | PROCESS_CREATE_THREAD
    | PROCESS_VM_OPERATION
    | PROCESS_VM_READ
)
MEM_COMMIT_RESERVE = 0x3000
PAGE_READWRITE = 0x04
MEM_RELEASE = 0x8000
MACHINE_I386 = 0x14C
MACHINE_AMD64 = 0x8664
DLL_WOW64 = "ThemeHook3_v56.dll"
DLL_NATIVE = "ThemeHook3_v56_x64.dll"
CONFIGURATOR_TITLE = "Конфигуратор"
IMAGE_NAME = "1cv8.exe"


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", wintypes.HMODULE),
        ("szModule", wintypes.WCHAR * 256),
        ("szExePath", wintypes.WCHAR * 260),
    ]


def path_inside(child: str, parent: str) -> bool:
    """True, если realpath файла лежит строго внутри каталога."""
    try:
        child_r = os.path.normcase(os.path.realpath(child))
        parent_r = os.path.normcase(os.path.realpath(parent))
    except OSError:
        return False
    if child_r == parent_r:
        return False
    try:
        return os.path.commonpath([child_r, parent_r]) == parent_r
    except ValueError:
        return False


def dll_inside_builds(path: str) -> bool:
    if not os.path.isfile(path):
        return False
    return path_inside(path, BUILDS_DIR)


def theme_module_path(name: str) -> str | None:
    """Имя файла темы из builds/. Чужое имя, путь и отсутствующий файл — None."""
    if not name or os.path.basename(name) != name:
        return None
    lower = name.lower()
    if not lower.startswith("themehook3_") or not lower.endswith(".dll"):
        return None
    path = os.path.join(BUILDS_DIR, name)
    if not dll_inside_builds(path):
        return None
    return os.path.realpath(path)


def dll_name_for_wow64(is_wow64: bool) -> str:
    return DLL_WOW64 if is_wow64 else DLL_NATIVE


def select_watcher_dll(is_wow64: bool, available: set[str]) -> str | None:
    """Имя DLL v56 нужной битности. Другую битность и v55 не подставляет."""
    name = dll_name_for_wow64(is_wow64)
    if name not in available:
        return None
    return name


def watcher_dll_path(is_wow64: bool) -> str | None:
    present = set(os.listdir(BUILDS_DIR)) if os.path.isdir(BUILDS_DIR) else set()
    name = select_watcher_dll(is_wow64, present)
    if name is None:
        return None
    return os.path.join(BUILDS_DIR, name)


def bitness_allowed(dll_machine: int, target_wow64: bool, pointer_size: int) -> bool:
    """Совпадение машины DLL, WOW64 цели и ширины указателя интерпретатора."""
    if pointer_size not in (4, 8):
        return False
    if not target_wow64 and pointer_size == 4:
        return False
    if dll_machine == MACHINE_I386:
        return bool(target_wow64)
    if dll_machine == MACHINE_AMD64:
        return (not target_wow64) and pointer_size == 8
    return False


def load_is_success(thread_exit_code: int, module_present: bool) -> bool:
    """Успех — только появление модуля. Код потока сам по себе успехом не считается."""
    del thread_exit_code
    return bool(module_present)


def unload_is_success(module_present: bool) -> bool:
    return not module_present


def remote_thread_allowed(theme_loaded: bool, export_status: str, bitness_ok: bool) -> bool:
    return bitness_ok and export_status == "ok" and not theme_loaded


def export_is_forwarder(rva: int, export_rva: int, export_size: int) -> bool:
    return export_size > 0 and export_rva <= rva < export_rva + export_size


def _pe_sections(data: bytes, pe: int) -> list[tuple[int, int, int]]:
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    section_off = pe + 24 + optsz
    sections: list[tuple[int, int, int]] = []
    for index in range(nsec):
        vs, va, rawsz, rawoff = struct.unpack_from("<IIII", data, section_off + index * 40 + 8)
        sections.append((va, max(vs, rawsz), rawoff))
    return sections


def _rva_to_off(sections: list[tuple[int, int, int]], rva: int) -> int | None:
    for va, size, rawoff in sections:
        if va <= rva < va + size:
            return rawoff + (rva - va)
    return None


def read_pe_machine(path: str) -> int:
    with open(path, "rb") as fh:
        data = fh.read(0x200)
    if len(data) < 0x40:
        return 0
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 6 > len(data):
        return 0
    return struct.unpack_from("<H", data, pe + 4)[0]


def resolve_export(data: bytes, export: str) -> tuple[str, int | None]:
    """('ok', rva), ('forwarder', rva) или ('missing', None).

    RVA внутри каталога экспорта — форвардер, поток по нему не создаётся.
    """
    try:
        if len(data) < 0x40:
            return ("missing", None)
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if pe + 26 > len(data):
            return ("missing", None)
        magic = struct.unpack_from("<H", data, pe + 24)[0]
        if magic == 0x10B:
            directory = pe + 24 + 96
        elif magic == 0x20B:
            directory = pe + 24 + 112
        else:
            return ("missing", None)
        export_rva, export_size = struct.unpack_from("<II", data, directory)
        if export_rva == 0 or export_size == 0:
            return ("missing", None)
        sections = _pe_sections(data, pe)
        export_off = _rva_to_off(sections, export_rva)
        if export_off is None:
            return ("missing", None)
        name_count = struct.unpack_from("<I", data, export_off + 0x18)[0]
        funcs_rva, names_rva, ords_rva = struct.unpack_from("<III", data, export_off + 0x1C)
        names_off = _rva_to_off(sections, names_rva)
        ords_off = _rva_to_off(sections, ords_rva)
        funcs_off = _rva_to_off(sections, funcs_rva)
        if None in (names_off, ords_off, funcs_off):
            return ("missing", None)
        needle = export.encode("ascii")
        for index in range(name_count):
            name_rva = struct.unpack_from("<I", data, names_off + index * 4)[0]
            name_off = _rva_to_off(sections, name_rva)
            if name_off is None:
                continue
            end = data.find(b"\0", name_off)
            if end < 0 or data[name_off:end] != needle:
                continue
            ordinal = struct.unpack_from("<H", data, ords_off + index * 2)[0]
            rva = struct.unpack_from("<I", data, funcs_off + ordinal * 4)[0]
            if export_is_forwarder(rva, export_rva, export_size):
                return ("forwarder", rva)
            return ("ok", rva)
    except (struct.error, IndexError, UnicodeError, OSError):
        return ("missing", None)
    return ("missing", None)


def image_name(pid: int) -> str | None:
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
            stderr=subprocess.DEVNULL,
        ).decode("cp866", errors="replace")
    except (subprocess.CalledProcessError, OSError):
        return None
    line = out.strip()
    lowered = line.lower()
    if not line or "no tasks" in lowered or "нет задач" in lowered:
        return None
    return line.split('","')[0].strip().strip('"')


def list_image_pids(name: str = IMAGE_NAME) -> list[int]:
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
            stderr=subprocess.DEVNULL,
        ).decode("cp866", errors="replace")
    except (subprocess.CalledProcessError, OSError):
        return []
    pids: list[int] = []
    for line in out.strip().splitlines():
        if name.lower() not in line.lower():
            continue
        try:
            pids.append(int(line.strip().strip('"').split('","')[1]))
        except (IndexError, ValueError):
            continue
    return pids


def has_configurator_window(pid: int, needle: str = CONFIGURATOR_TITLE) -> bool:
    """Видимое top-level окно процесса, в заголовке которого есть «Конфигуратор»."""
    user32 = ctypes.windll.user32
    found = {"hwnd": 0}
    proto = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def callback(hwnd, _lp):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        text = ctypes.create_unicode_buffer(512)
        user32.GetWindowTextW(hwnd, text, 512)
        if needle in text.value:
            found["hwnd"] = int(hwnd)
            return False
        return True

    enum_cb = proto(callback)
    user32.EnumWindows(enum_cb, 0)
    return bool(found["hwnd"])


def is_configurator(pid: int) -> bool:
    name = image_name(pid)
    if not name or name.lower() != IMAGE_NAME:
        return False
    return has_configurator_window(pid)


def configurator_pids() -> list[int]:
    return [pid for pid in list_image_pids() if has_configurator_window(pid)]


def is_wow64(handle) -> bool | None:
    wow = wintypes.BOOL()
    if not k.IsWow64Process(handle, ctypes.byref(wow)):
        return None
    return bool(wow.value)


def process_is_wow64(pid: int) -> bool | None:
    handle = k.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        return is_wow64(handle)
    finally:
        k.CloseHandle(handle)


def snapshot_modules(pid: int) -> tuple[bool, list[tuple[str, int]]]:
    snap = k.CreateToolhelp32Snapshot(0x8 | 0x10, pid)
    invalid = ctypes.c_void_p(-1).value
    if not snap or snap == invalid:
        return (False, [])
    entries: list[tuple[str, int]] = []
    try:
        me = MODULEENTRY32W()
        me.dwSize = ctypes.sizeof(MODULEENTRY32W)
        if not k.Module32FirstW(snap, ctypes.byref(me)):
            return (False, [])
        while True:
            base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value or 0
            entries.append((me.szModule, int(base)))
            if not k.Module32NextW(snap, ctypes.byref(me)):
                break
    finally:
        k.CloseHandle(snap)
    return (True, entries)


def theme_loaded(names: list[str]) -> bool:
    for name in names:
        lower = name.lower()
        if lower.startswith("themehook3_") and lower.endswith(".dll"):
            return True
    return False


def module_present(names: list[str], module_name: str) -> bool:
    want = module_name.lower()
    return any(name.lower() == want for name in names)


def module_base(pid: int, module_name: str) -> int | None:
    ok, entries = snapshot_modules(pid)
    if not ok:
        return None
    for name, base in entries:
        if name.lower() == module_name.lower():
            return base
    return None


def remote_kernel_export(pid: int, wow: bool, export: str) -> tuple[str, int | None]:
    folder = "SysWOW64" if wow else "System32"
    path = os.path.join(os.environ["SystemRoot"], folder, "kernel32.dll")
    with open(path, "rb") as fh:
        data = fh.read()
    status, rva = resolve_export(data, export)
    if status != "ok" or rva is None:
        return (status, None)
    ok, entries = snapshot_modules(pid)
    if not ok:
        return ("missing", None)
    for name, base in entries:
        if name.lower() == "kernel32.dll":
            return ("ok", base + rva)
    return ("missing", None)


def _close_remote(handle, thread, remote) -> None:
    if thread:
        k.CloseHandle(thread)
    if remote:
        k.VirtualFreeEx(handle, remote, 0, MEM_RELEASE)
    if handle:
        k.CloseHandle(handle)


def inject_dll(pid: int, dll: str) -> int:
    """Загрузить DLL в конфигуратор. Отказ до OpenProcess, если путь или окно не те."""
    dll = os.path.abspath(dll)
    if not os.path.isfile(dll):
        print(f"[!] DLL не найден: {dll}")
        return 2
    if not dll_inside_builds(dll):
        print(f"[!] путь DLL вне builds/: {dll}")
        return 2
    if not is_configurator(pid):
        print(f"[!] pid={pid} не 1cv8.exe с видимым окном «Конфигуратор»")
        return 3

    handle = k.OpenProcess(INJECT_ACCESS, False, pid)
    if not handle:
        print(f"[!] OpenProcess failed, err={k.GetLastError()}")
        return 4
    remote = None
    thread = None
    try:
        wow = is_wow64(handle)
        if wow is None:
            print(f"[!] IsWow64Process failed, err={k.GetLastError()}")
            return 4
        machine = read_pe_machine(dll)
        pointer_size = ctypes.sizeof(ctypes.c_void_p)
        allowed = bitness_allowed(machine, wow, pointer_size)
        if not allowed:
            print(
                f"[!] битность не совпала: machine={machine:#x} wow64={wow} "
                f"pointer={pointer_size}"
            )
            return 9
        ok, entries = snapshot_modules(pid)
        if not ok:
            print("[!] снимок модулей не удался")
            return 4
        names = [name for name, _base in entries]
        if theme_loaded(names):
            print("[!] тема уже загружена, поток не создаётся")
            return 10
        status, loadlib = remote_kernel_export(pid, wow, "LoadLibraryW")
        if not remote_thread_allowed(False, status, True) or loadlib is None:
            print(f"[!] LoadLibraryW отклонён ({status}), поток не создаётся")
            return 11
        path_bytes = (dll + "\0").encode("utf-16-le")
        remote = k.VirtualAllocEx(handle, None, len(path_bytes), MEM_COMMIT_RESERVE, PAGE_READWRITE)
        if not remote:
            print(f"[!] VirtualAllocEx failed, err={k.GetLastError()}")
            return 5
        written = ctypes.c_size_t(0)
        wrote = k.WriteProcessMemory(
            handle, remote, path_bytes, len(path_bytes), ctypes.byref(written),
        )
        if not wrote:
            print(f"[!] WriteProcessMemory failed, err={k.GetLastError()}")
            return 6
        tid = wintypes.DWORD(0)
        thread = k.CreateRemoteThread(
            handle, None, 0, ctypes.c_void_p(loadlib), ctypes.c_void_p(remote),
            0, ctypes.byref(tid),
        )
        if not thread:
            print(f"[!] CreateRemoteThread failed, err={k.GetLastError()}")
            return 7
        k.WaitForSingleObject(thread, 15000)
        code = wintypes.DWORD(0)
        k.GetExitCodeThread(thread, ctypes.byref(code))
        ok, entries = snapshot_modules(pid)
        present = ok and module_present([name for name, _base in entries], os.path.basename(dll))
        if not ok or not load_is_success(code.value, present):
            print(f"[!] модуль темы не появился в процессе (код потока 0x{code.value:08X})")
            return 8
        print(f"[+] injection OK, pid={pid}, module={os.path.basename(dll)}")
        return 0
    finally:
        _close_remote(handle, thread, remote)


def unload_module(pid: int, module_name: str) -> int:
    """Снять модуль темы. Чужое имя не доходит до OpenProcess и FreeLibrary."""
    if theme_module_path(module_name) is None:
        print(f"[!] выгрузка отклонена: {module_name} не файл темы в builds/")
        return 2
    if not is_configurator(pid):
        print(f"[!] pid={pid} не 1cv8.exe с видимым окном «Конфигуратор»")
        return 3
    handle = k.OpenProcess(UNLOAD_ACCESS, False, pid)
    if not handle:
        print(f"[!] OpenProcess failed, err={k.GetLastError()}")
        return 4
    thread = None
    try:
        wow = is_wow64(handle)
        if wow is None:
            print(f"[!] IsWow64Process failed, err={k.GetLastError()}")
            return 4
        pointer_size = ctypes.sizeof(ctypes.c_void_p)
        if not wow and pointer_size == 4:
            print("[!] 32-битный Python не создаёт поток в нативном x64")
            return 9
        ok, entries = snapshot_modules(pid)
        if not ok:
            print("[!] снимок модулей не удался")
            return 4
        base = next((addr for name, addr in entries if name.lower() == module_name.lower()), None)
        if not base:
            print(f"[!] {module_name} не загружен в pid={pid}")
            return 4
        status, freelib = remote_kernel_export(pid, wow, "FreeLibrary")
        if not remote_thread_allowed(False, status, True) or freelib is None:
            print(f"[!] FreeLibrary отклонён ({status}), поток не создаётся")
            return 11
        tid = wintypes.DWORD(0)
        thread = k.CreateRemoteThread(
            handle, None, 0, ctypes.c_void_p(freelib), ctypes.c_void_p(base), 0, ctypes.byref(tid),
        )
        if not thread:
            print(f"[!] CreateRemoteThread failed, err={k.GetLastError()}")
            return 7
        k.WaitForSingleObject(thread, 10000)
        ok, entries = snapshot_modules(pid)
        present = ok and module_present([name for name, _base in entries], module_name)
        if not ok or not unload_is_success(present):
            print(f"[!] {module_name} всё ещё в процессе")
            return 5
        print(f"[+] unloaded {module_name} from pid={pid}")
        return 0
    finally:
        _close_remote(handle, thread, None)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dll", required=True)
    parser.add_argument("--pid", type=int, default=None)
    args = parser.parse_args(argv)
    dll = os.path.abspath(args.dll)
    if not os.path.isfile(dll):
        print(f"[!] DLL не найден: {dll}")
        return 2
    if not dll_inside_builds(dll):
        print(f"[!] путь DLL вне builds/: {dll}")
        return 2
    if args.pid is not None:
        return inject_dll(args.pid, dll)
    pids = configurator_pids()
    if not pids:
        print("[!] нет 1cv8.exe с окном «Конфигуратор»")
        return 3
    machine = read_pe_machine(dll)
    pointer_size = ctypes.sizeof(ctypes.c_void_p)
    chosen = None
    for pid in pids:
        wow = process_is_wow64(pid)
        if wow is None:
            continue
        if bitness_allowed(machine, wow, pointer_size):
            chosen = pid
            break
    if chosen is None:
        print("[!] нет конфигуратора той же битности, поток не создаётся")
        return 9
    return inject_dll(chosen, dll)


if __name__ == "__main__":
    sys.exit(main())
