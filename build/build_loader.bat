@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /LD /MT /O2 /W3 ..\src\ThemeLoader.cpp /Fe:..\builds\ThemeLoader.dll /link kernel32.lib /DEF:..\src\ThemeLoader.def /NODEFAULTLIB:msvcrt
