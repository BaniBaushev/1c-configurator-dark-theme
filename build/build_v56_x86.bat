@echo off
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINST="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINST=%%i"
if not defined VSINST (
  echo Visual C++ tools not found
  exit /b 1
)
call "%VSINST%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cl /utf-8 /LD /MD /O2 /W3 ..\src\ThemeHook3.cpp /Fe:..\builds\ThemeHook3_v56.dll /link user32.lib gdi32.lib psapi.lib /DEF:..\src\ThemeHook3.def
exit /b %ERRORLEVEL%
