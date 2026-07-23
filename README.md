# 1C Configurator Theme Engine

Тёмная тема (EDT Dark) для конфигуратора 1С — **без модификации файлов платформы**. DLL внедряется в процесс 1cv8.exe и перекрашивает интерфейс на лету; выгрузка возвращает светлую тему без перезапуска.

**Версия: v5.5** · 1cv8.exe x86 · MSVC 2022 · Python-инжектор

| Референс (1C:EDT) | Результат (v5.5) |
|---|---|
| ![reference](screenshots/reference.png) | ![verify_dark_v55](screenshots/verify_dark_v55_repaint.png) |

## Принцип работы

1С рисует весь UI сама (owner-draw) через **Cairo** (`grphcs.dll` → `cairo.dll`), справка — через WebKit. Поэтому тема перехватывает вызовы рендера и подменяет цвета до отрисовки:

- **EAT-патч** таблиц экспорта `cairo.dll` / `gdi32.dll` / `user32.dll` — хуки получают все модули, включая загруженные после инжекции (F1-справка, диалоги); IAT-патч и фоновый re-scan — страховка.
- **Хуки Cairo:** `cairo_set_source_rgb/rgba` (маппинг фонов; clip-контекст отличает дерево `#333333` от редактора `#1E1E1E`), `cairo_show_glyphs/show_text` (цвет текста + пропуск дублей emboss/bold через ring history).
- **Хуки GDI/USER:** `SetTextColor`, `ExtTextOutW`, `DrawTextW`, `CreateSolidBrush`, `FillRect`, `GetSysColor(Brush)` — ролевой маппинг (выделение и т.п.).
- **Карта цветов:** 24+ явных записей (near-match ±6) + HSL-fallback (инверсия lightness) + pale-tint правило.
- **Безопасная выгрузка:** все патчи журналируются и откатываются при `FreeLibrary` — процесс 1С не падает.

Инвариант **SKIP-ONLY**: дорисовывать в cairo-контексты и править пиксели кэшированных surface нельзя — изменения запекаются в кэш 1С/WebKit и переживают выгрузку DLL. Только пропуск дублей или перекраска до отрисовки.

Подробная документация — в [PROJECT_DOCS.md](docs/PROJECT_DOCS.md).

## Что сделано (история версий)

| Версия | Суть | Итог |
|--------|------|------|
| v1 | Хуки FillRect/SetBkColor | Нулевой эффект — 1С не рисует UI классическим GDI |
| v2 | + `cairo_set_source_rgb/rgba` | **Прорыв:** весь UI потемнел |
| v3 | Точная палитра + HSL-fallback + IAT-restore | FreeLibrary без отката IAT = краш; откат обязателен |
| v4.x | Clip-контекст, палитра EDT, ghost-fix emboss, карта лексем | Текст и код читаемы |
| v4.8 | Проверка гипотезы кэш-паттернов | Опровергнута; размытие заголовков — врождённый bold WebKit |
| v4.9 | RescanThread (F1), GDI ghost-fix (меню), COLOR_HIGHLIGHT | Поздние DLL требуют пере-патчинга |
| **v5.0** | **EAT-патч** cairo/gdi32/user32 | Поздние модули хукаются мгновенно; F1 тёмный сразу |
| **v5.1** | Pale-tint классификация | Жёлтая шапка F1 исправлена |
| **v5.2–5.3** | Ring-history suppression дублей (cairo + GDI) | Emboss и синтетический bold пропускаются |
| **v5.4** | Режим SKIP-ONLY, удалены redraw/surf-remap | Устранено отравление кэш-поверхностей |
| **v5.5** | Watcher-автоинжекция + урок ранней инжекции | Тема применяется сама, безопасно (см. ниже) |

> ⚠️ **Урок v5.5 (2026-07-24):** инжектировать тему в стартующий процесс 1С НЕЛЬЗЯ — хуки перекрашивают текст в момент построения стартовых кэшей отрисовки, перекрашенные прогоны запекаются в кэш и двоение переживает даже полную выгрузку DLL. Инвариант SKIP-ONLY дополнен: хуки не должны работать **в момент создания кэша**. Watcher ждёт окно «Конфигуратор» + 25 с и только потом инжектит. Проверено живьём: тёмная чёткая, выгрузка чистая (`verify_dark_v55_repaint.png`, `verify_light_unloaded.png`).

## Дорожная карта

1. ~~Watcher-автоинжекция~~ — ✅ **сделано (v5.5)**: `tools/theme_watcher.py` применяет тему к каждому 1cv8.exe автоматически (после появления окна «Конфигуратор» + 25 с; ранняя инжекция запрещена — см. урок выше). Автозапуск: `tools/watcher_install.bat`.
2. Перерисовка без ресайза окна (InvalidateRect / WM_SETTINGCHANGE).
3. Проверка диалогов и форм объектов, дополнение карты цветов.
4. JSON-профили тем (Dracula / One Dark / Monokai / custom).
5. GUI менеджера профилей (C# WPF).

Актуальные задачи — [TODO.md](docs/TODO.md).

## Структура репозитория

```
src/          исходники (ThemeHook3.cpp v5.5, PaletteLog.cpp, ThemeLoader.cpp — архив IFEO)
build/        build-скрипты (MSVC 2022, x86) — запускать из этой папки
builds/       собранные DLL (ThemeHook3_v50…v55.dll и др.)
tools/        inject.py / unload.py / screenshot.py / theme_watcher.py и др.
docs/         PROJECT_DOCS.md (полная документация), TODO.md
screenshots/  reference.png (цель EDT), verify_dark_v55_repaint.png (результат)
```

## Использование

Автоматически (рекомендуется):
```bat
tools/watcher_install.bat    :: watcher в автозагрузку; тема сама применяется к каждому 1cv8.exe
```

Вручную:
```bat
build/build_v55.bat                          :: сборка (x86 Native Tools, MSVC 2022)
python tools/inject.py --dll builds\ThemeHook3_v55.dll   :: применить тему к запущенному 1cv8.exe
:: ⚠️ инжектировать только в полностью загрузившийся конфигуратор (окно открыто, ~30 с после старта)
:: потянуть окно конфигуратора за угол — триггер перерисовки
python tools/unload.py --module ThemeHook3_v55.dll       :: выгрузить тему (процесс выживает)
```
