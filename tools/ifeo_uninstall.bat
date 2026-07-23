@echo off
:: Откат IFEO-автозагрузки ThemeHook3 для 1cv8.exe.
:: Требуются права администратора (запись в HKLM).
setlocal
set "KEY=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\1cv8.exe"

net session >nul 2>&1
if errorlevel 1 (
    echo Запрос прав администратора...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b 0
)

reg delete "%KEY%" /v VerifierDlls /f
reg delete "%KEY%" /v GlobalFlag /f

echo.
echo [OK] IFEO для 1cv8.exe удалён. Конфигуратор запускается без темы.
endlocal
