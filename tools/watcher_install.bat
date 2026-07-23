@echo off
:: Автозапуск Theme Watcher при входе в Windows (HKCU Run, права админа НЕ нужны).
:: Откат: tools\watcher_uninstall.bat
setlocal
set "SCRIPT=%~dp0theme_watcher.py"
for %%I in ("%SCRIPT%") do set "SCRIPT=%%~fI"

:: pythonw из managed-рантайма Kimi; при переносе проекта поправить путь
where pythonw >nul 2>&1
if errorlevel 1 (
    echo [ОШИБКА] pythonw не найден в PATH
    exit /b 1
)

reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "1CThemeWatcher" /t REG_SZ /d "pythonw \"%SCRIPT%\"" /f

echo.
echo [OK] Watcher добавлен в автозапуск (HKCU Run).
echo Запустить сейчас без перелогина:  pythonw "%SCRIPT%"
echo Лог: logs\watcher_log.txt
endlocal
