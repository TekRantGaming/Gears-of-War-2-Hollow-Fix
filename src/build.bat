@echo off
rem Builds the 32-bit proxy DLLs (the game is a 32-bit exe). Needs VS 2022 Build Tools.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /O2 /W3 /MT /LD /DHIF_XINPUT HollowInputFix.c /Fo:out\xi.obj /Fe:out\xinput1_3.dll /link /DEF:xinput1_3.def user32.lib kernel32.lib || exit /b 1
cl /nologo /O2 /W3 /MT /LD /DHIF_DINPUT HollowInputFix.c /Fo:out\di.obj /Fe:out\dinput8.dll /link /DEF:dinput8.def user32.lib kernel32.lib || exit /b 1
echo Built out\xinput1_3.dll and out\dinput8.dll
