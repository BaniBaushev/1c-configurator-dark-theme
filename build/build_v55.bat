@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /LD /MD /O2 /W3 ..\src\ThemeHook3.cpp /Fe:..\builds\ThemeHook3_v55.dll /link user32.lib gdi32.lib psapi.lib /DEF:..\src\ThemeHook3.def
