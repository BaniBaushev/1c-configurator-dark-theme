@echo off
REM Build ThemeHook.dll x86 with MSVC 2022
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /LD /MD /O2 /W3 ThemeHook.cpp /Fe:ThemeHook.dll /link user32.lib gdi32.lib psapi.lib
if errorlevel 1 exit /b 1
echo OK: ThemeHook.dll
