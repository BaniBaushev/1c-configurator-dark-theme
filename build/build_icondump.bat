@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /LD /MD /O2 /W3 ..\src\IconDump.cpp /Fe:..\builds\IconDump.dll /link user32.lib gdi32.lib
