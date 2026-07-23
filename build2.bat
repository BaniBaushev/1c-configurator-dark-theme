@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >/dev/null
cl /LD /MD /O2 /W3 %1 /Fe:%2 /link user32.lib gdi32.lib psapi.lib
