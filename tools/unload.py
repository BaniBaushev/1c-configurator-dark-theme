# -*- coding: utf-8 -*-
"""Unload a DLL from a remote (possibly wow64/32-bit) process via FreeLibrary."""
import argparse, ctypes, os, struct, subprocess, sys
from ctypes import wintypes
import inject

k = inject.k

def rva_of_export(dll_path, export):
    data = open(dll_path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    secs = []
    so = pe + 24 + optsz
    for i in range(nsec):
        vs, va, rawsz, rawoff = struct.unpack_from("<IIII", data, so + i * 40 + 8)
        secs.append((va, max(vs, rawsz), rawoff))
    def r2o(rva):
        for va, sz, rawoff in secs:
            if va <= rva < va + sz:
                return rawoff + rva - va
        return rva
    eo = r2o(struct.unpack_from("<I", data, pe + 0x78)[0])
    n_funcs, n_names = struct.unpack_from("<II", data, eo + 0x14)
    funcs, names, ords = struct.unpack_from("<III", data, eo + 0x1C)
    for i in range(n_names):
        no = r2o(struct.unpack_from("<I", data, r2o(names) + i * 4)[0])
        if data[no:data.index(b"\0", no)] == export.encode():
            oi = struct.unpack_from("<H", data, r2o(ords) + i * 2)[0]
            return struct.unpack_from("<I", data, r2o(funcs) + oi * 4)[0]
    raise RuntimeError(f"export {export} not found")

class ME32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
                ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
                ("modBaseSize", wintypes.DWORD), ("hModule", wintypes.HMODULE),
                ("szModule", wintypes.WCHAR * 256), ("szExePath", wintypes.WCHAR * 260)]

def find_module(pid, name):
    snap = k.CreateToolhelp32Snapshot(0x8 | 0x10, pid)
    me = ME32(); me.dwSize = ctypes.sizeof(ME32)
    base = None
    if k.Module32FirstW(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == name.lower():
                base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value
                break
            if not k.Module32NextW(snap, ctypes.byref(me)):
                break
    k.CloseHandle(snap)
    return base

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--module", default="ThemeHook3_v55.dll")
    ap.add_argument("--pid", type=int, default=None)
    a = ap.parse_args()
    pid = a.pid or inject.find_pid()
    if not pid:
        print("[!] 1cv8.exe not running"); return 3
    h = k.OpenProcess(0x1F0FFF, False, pid)
    wow = inject.is_wow64(h)
    mod_base = find_module(pid, a.module)
    if not mod_base:
        print(f"[!] {a.module} not loaded in pid={pid}"); return 4
    k32_path = os.path.join(os.environ["SystemRoot"],
                            "SysWOW64" if wow else "System32", "kernel32.dll")
    freelib = find_module(pid, "kernel32.dll") + rva_of_export(k32_path, "FreeLibrary")
    tid = wintypes.DWORD(0)
    th = k.CreateRemoteThread(h, None, 0, ctypes.c_void_p(freelib),
                              ctypes.c_void_p(mod_base), 0, ctypes.byref(tid))
    k.WaitForSingleObject(th, 10000)
    code = wintypes.DWORD(0); k.GetExitCodeThread(th, ctypes.byref(code))
    k.CloseHandle(th); k.CloseHandle(h)
    print(f"[+] FreeLibrary({a.module} @ 0x{mod_base:X}) -> {code.value}")
    return 0 if code.value else 5

if __name__ == "__main__":
    sys.exit(main())
