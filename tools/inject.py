#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Inject ThemeHook.dll into 1cv8.exe via CreateRemoteThread(LoadLibraryW). Pure ctypes."""
import argparse, ctypes, os, subprocess, sys
from ctypes import wintypes

k = ctypes.windll.kernel32
k.GetProcAddress.restype = ctypes.c_void_p
k.GetProcAddress.argtypes = [wintypes.HMODULE, wintypes.LPCSTR]
k.GetModuleHandleW.restype = wintypes.HMODULE
k.VirtualAllocEx.restype = ctypes.c_void_p
k.OpenProcess.restype = wintypes.HANDLE
k.CreateRemoteThread.restype = wintypes.HANDLE
k.CreateRemoteThread.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                 ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
                                 ctypes.POINTER(wintypes.DWORD)]

PROCESS_ALL_ACCESS_NEEDED = 0x1F0FFF
MEM_COMMIT_RESERVE = 0x3000
PAGE_READWRITE = 0x04

def find_pid(name="1cv8.exe"):
    out = subprocess.check_output(
        ["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"]
    ).decode("cp866", errors="replace")
    for line in out.strip().splitlines():
        if name.lower() in line.lower():
            return int(line.strip().strip('"').split('","')[1])
    return None

def is_wow64(h):
    wow = wintypes.BOOL()
    k.IsWow64Process(h, ctypes.byref(wow))
    return bool(wow.value)

def remote_loadlib_addr(h, wow, pid):
    """LoadLibraryW address in the target process (handles wow64/32-bit targets)."""
    if not wow:
        hKernel = k.GetModuleHandleW("kernel32.dll")
        return k.GetProcAddress(hKernel, b"LoadLibraryW")
    # 32-bit target: RVA of LoadLibraryW in 32-bit kernel32.dll + remote kernel32 base
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
    export_rva = struct.unpack_from("<I", data, pe_off + 0x78)[0]
    eo = r2o(export_rva)
    n_funcs, n_names = struct.unpack_from("<II", data, eo + 0x14)
    funcs_rva, names_rva, ords_rva = struct.unpack_from("<III", data, eo + 0x1C)
    rva = None
    for i in range(n_names):
        name_off = r2o(struct.unpack_from("<I", data, r2o(names_rva) + i * 4)[0])
        name = data[name_off:data.index(b"\0", name_off)].decode()
        if name == "LoadLibraryW":
            ord_i = struct.unpack_from("<H", data, r2o(ords_rva) + i * 2)[0]
            rva = struct.unpack_from("<I", data, r2o(funcs_rva) + ord_i * 4)[0]
            break
    assert rva, "LoadLibraryW RVA not found in SysWOW64 kernel32.dll"
    # find kernel32.dll base in remote process
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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True)
    ap.add_argument("--pid", type=int, default=None)
    a = ap.parse_args()

    dll = os.path.abspath(a.dll)
    if not os.path.isfile(dll):
        print(f"[!] DLL not found: {dll}"); return 2

    pid = a.pid or find_pid()
    if not pid:
        print("[!] 1cv8.exe not running"); return 3

    h = k.OpenProcess(PROCESS_ALL_ACCESS_NEEDED, False, pid)
    if not h:
        print(f"[!] OpenProcess failed, err={k.GetLastError()}"); return 4

    wow = is_wow64(h)
    print(f"[i] target pid={pid}, wow64={wow} -> process is {'32-bit' if wow else '64-bit'}")
    if wow:
        print("[i] target is 32-bit, DLL must be x86")
    else:
        print("[i] target is 64-bit, DLL must be x64")

    path_bytes = (dll + "\0").encode("utf-16-le")
    size = len(path_bytes)

    addr = k.VirtualAllocEx(h, None, size, MEM_COMMIT_RESERVE, PAGE_READWRITE)
    if not addr:
        print(f"[!] VirtualAllocEx failed, err={k.GetLastError()}"); return 5
    written = ctypes.c_size_t(0)
    ok = k.WriteProcessMemory(h, addr, path_bytes, size, ctypes.byref(written))
    if not ok:
        print(f"[!] WriteProcessMemory failed, err={k.GetLastError()}"); return 6
    print(f"[i] wrote {written.value} bytes at 0x{addr:X}")

    loadlib = remote_loadlib_addr(h, wow, pid)
    print(f"[i] remote LoadLibraryW @ 0x{loadlib:08X}")

    tid = wintypes.DWORD(0)
    th = k.CreateRemoteThread(h, None, 0, loadlib, addr, 0, ctypes.byref(tid))
    if not th:
        print(f"[!] CreateRemoteThread failed, err={k.GetLastError()} (antivirus?)"); return 7
    k.WaitForSingleObject(th, 15000)
    code = wintypes.DWORD(0)
    k.GetExitCodeThread(th, ctypes.byref(code))
    print(f"[+] remote thread {tid.value} exited with code 0x{code.value:08X} (hModule of injected DLL)")
    if code.value == 0:
        print("[!] LoadLibraryW returned NULL - DLL failed to load")
        return 8
    k.CloseHandle(th)
    k.VirtualFreeEx(h, addr, 0, 0x8000)  # MEM_RELEASE
    k.CloseHandle(h)
    print("[+] injection OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())
