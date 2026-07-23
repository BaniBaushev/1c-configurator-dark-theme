@echo off
:: Откат автозапуска Theme Watcher.
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "1CThemeWatcher" /f
echo [OK] Watcher удалён из автозапуска. Работающий процесс pythonw остановите вручную (Диспетчер задач).
