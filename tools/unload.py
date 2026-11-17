# -*- coding: utf-8 -*-
"""Выгрузка модуля темы из конфигуратора 1С через FreeLibrary."""
from __future__ import annotations

import argparse
import sys

import inject


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--module", default="ThemeHook3_v56.dll")
    parser.add_argument("--pid", type=int, default=None)
    args = parser.parse_args(argv)
    if inject.theme_module_path(args.module) is None:
        print(f"[!] выгрузка отклонена: {args.module} не файл темы в builds/")
        return 2
    if args.pid is not None:
        return inject.unload_module(args.pid, args.module)
    loaded = []
    for pid in inject.configurator_pids():
        ok, entries = inject.snapshot_modules(pid)
        if ok and inject.module_present([name for name, _base in entries], args.module):
            loaded.append(pid)
    if not loaded:
        print(f"[!] {args.module} не загружен в конфигуратор")
        return 4
    if len(loaded) > 1:
        print("[!] несколько конфигураторов с модулем, укажите --pid")
        return 3
    return inject.unload_module(loaded[0], args.module)


if __name__ == "__main__":
    sys.exit(main())
