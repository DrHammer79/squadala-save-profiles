@echo off
rem Builds mod\save_profiles.dll from src\.
rem   build.cmd                       this folder sits inside the co-op kit, or a compiler is on PATH
rem   build.cmd "C:\path\to\the kit"  use the clang the kit downloaded for Longshot
rem The compiler, first found: %CC%; the kit's hookshot\tools\llvm-mingw clang; x86_64-w64-mingw32-clang or -gcc on PATH.
setlocal
set "HERE=%~dp0"
set "KIT=%~1"
if "%KIT%"=="" set "KIT=%HERE%.."
set "KITCC=%KIT%\hookshot\tools\llvm-mingw\bin\x86_64-w64-mingw32-clang.exe"
if not defined CC if exist "%KITCC%" set "CC=%KITCC%"
if not defined CC for %%c in (x86_64-w64-mingw32-clang.exe x86_64-w64-mingw32-gcc.exe) do if not defined CC if not "%%~$PATH:c"=="" set "CC=%%~$PATH:c"
if not defined CC (
  echo No compiler found. Either:
  echo   build.cmd "C:\path\to\the co-op kit"   ^(the kit downloads llvm-mingw the first time it builds Longshot^)
  echo   or put llvm-mingw's bin folder on PATH, or set CC to a MinGW-w64 clang or gcc.
  exit /b 1
)
"%CC%" -O2 -Wall -Wextra -Wno-unused-parameter -shared -o "%HERE%mod\save_profiles.dll" "%HERE%src\save_profiles.c" -Wl,--no-insert-timestamp
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
echo Built %HERE%mod\save_profiles.dll with %CC%
echo Run install.cmd to put it into the game.
