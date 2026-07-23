@echo off
:: [ЗАБЛОКИРОВАНО 2026-07-23] Эксперимент показал: verifier-DLL => краш 0xc0000142.
:: Скрипт оставлен как артефакт; не использовать без перепроверки на целевой ОС.
:: IFEO-автозагрузка ThemeHook3 при старте 1cv8.exe (гипотеза C).
:: Windows сама загружает DLL при запуске конфигуратора: без инжектора,
:: без CreateRemoteThread, хуки активны с нулевой секунды.
:: Требуются права администратора (запись в HKLM).
setlocal
set "KEY=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\1cv8.exe"
set "DLL=%~dp0..\builds\ThemeLoader.dll"
for %%I in ("%DLL%") do set "DLL=%%~fI"

if not exist "%DLL%" (
    echo [ОШИБКА] DLL не найдена: %DLL%
    echo Сначала соберите: build\build_v54.bat
    exit /b 1
)

net session >nul 2>&1
if errorlevel 1 (
    echo Запрос прав администратора...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b 0
)

reg add "%KEY%" /v GlobalFlag /t REG_DWORD /d 0x100 /f
reg add "%KEY%" /v VerifierDlls /t REG_SZ /d "%DLL%" /f

echo.
echo [OK] IFEO установлен для 1cv8.exe:
reg query "%KEY%"
echo.
echo Теперь тема будет загружаться автоматически при каждом запуске конфигуратора.
echo Откат: tools\ifeo_uninstall.bat
endlocal
