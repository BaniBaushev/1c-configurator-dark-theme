#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Ручной watcher: после окна «Конфигуратор» и паузы 25 с грузит DLL v56 той же битности.

ThemeHook3_v56.dll — процесс WOW64, ThemeHook3_v56_x64.dll — нативный x64.
Нет файла нужной битности — pid пропускается; другая DLL и ThemeHook v55 не подставляются.
Ключ автозагрузки и IFEO этой поставкой не ставятся. Запуск только вручную.
"""
from __future__ import annotations

import os
import sys
import time

import inject

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOG = os.path.join(ROOT, "logs", "watcher_log.txt")
POLL_SEC = 2.0
SETTLE_SEC = 25.0


def log(msg: str) -> None:
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    print(line, flush=True)
    try:
        os.makedirs(os.path.dirname(LOG), exist_ok=True)
        with open(LOG, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
    except OSError:
        pass


def main() -> int:
    log("watcher стартовал вручную; автозагрузка и IFEO не ставятся")
    seen: dict[int, float] = {}
    ready: dict[int, float] = {}
    done: set[int] = set()
    try:
        while True:
            pids = inject.list_image_pids()
            now = time.time()
            for pid in pids:
                if pid in done:
                    continue
                if not inject.has_configurator_window(pid):
                    ready.pop(pid, None)
                    if pid not in seen:
                        seen[pid] = now
                        log(f"pid={pid}: жду окно «Конфигуратор»")
                    continue
                if pid not in ready:
                    ready[pid] = now
                    log(f"pid={pid}: окно найдено, жду {SETTLE_SEC:.0f} с")
                    continue
                if now - ready[pid] < SETTLE_SEC:
                    continue
                wow = inject.process_is_wow64(pid)
                if wow is None:
                    log(f"pid={pid}: битность неизвестна, пропуск")
                    done.add(pid)
                    continue
                dll = inject.watcher_dll_path(wow)
                if dll is None:
                    name = inject.dll_name_for_wow64(wow)
                    log(f"pid={pid}: нет {name}, другую DLL не подставляю")
                    done.add(pid)
                    continue
                code = inject.inject_dll(pid, dll)
                log(f"pid={pid}: inject {os.path.basename(dll)} -> {code}")
                done.add(pid)
                seen.pop(pid, None)
                ready.pop(pid, None)
            alive = set(pids)
            for pid in list(seen):
                if pid not in alive:
                    seen.pop(pid, None)
                    ready.pop(pid, None)
                    done.discard(pid)
            for pid in list(done):
                if pid not in alive:
                    done.discard(pid)
            time.sleep(POLL_SEC)
    except KeyboardInterrupt:
        log("watcher остановлен пользователем")
    return 0


if __name__ == "__main__":
    sys.exit(main())
