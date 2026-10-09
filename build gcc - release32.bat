@echo off
REM This script deliberately calls a NATIVE WINDOWS cmake by full path instead of
REM whatever `cmake` resolves to. devkitPro's installer puts its MSYS2 bin
REM directory on the system PATH ahead of everything else, so a bare `cmake` is
REM the MSYS2 build even from cmd.exe. That one emits POSIX paths (/d/...)
REM into build.ninja -- and the build dies inside CMake's compiler check with a
REM misleading "the C compiler is broken" when the compiler is perfectly fine.
REM See build wii.bat for the same fix applied to the Wii preset.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "CMAKE_EXE="
for %%C in (
    "%ProgramFiles%\CMake\bin\cmake.exe"
    "%ProgramFiles(x86)%\CMake\bin\cmake.exe"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) do (
    if not defined CMAKE_EXE if exist %%C set "CMAKE_EXE=%%~C"
)
if not defined CMAKE_EXE (
    echo WARNING: no native Windows cmake found; falling back to whatever is on PATH.
    echo          If that is devkitPro's MSYS2 cmake the build will fail in the
    echo          compiler check - see the note at the top of this file.
    set "CMAKE_EXE=cmake"
)
echo Using cmake: !CMAKE_EXE!

if /I "%~1"=="clean" (
    if exist "build\gcc32-release" rmdir /s /q "build\gcc32-release"
)

REM --- toolchain: 32-bit gcc + ninja ---------------------------------------
REM The gcc32-* presets pass -m32, so they need a 32-bit-capable gcc (MSYS2
REM mingw32) -- a 64-bit gcc on PATH cannot do it. Prefer C:\msys64\mingw32,
REM then verify whatever gcc resolves accepts -m32 before CMake wastes a
REM configure on it. Ninja is passed explicitly via -DCMAKE_MAKE_PROGRAM:
REM the presets no longer pin it. Any ninja binary drives a 32-bit build.
if exist "C:\msys64\mingw32\bin\gcc.exe" (
    set "PATH=C:\msys64\mingw32\bin;!PATH!"
    echo Using gcc from: C:\msys64\mingw32\bin
)
gcc -m32 -E - <nul >nul 2>nul
if errorlevel 1 (
    echo ERROR: no 32-bit-capable gcc ^(needs -m32; tried PATH and C:\msys64\mingw32\bin^).
    echo        Install it with: pacman -S mingw-w64-i686-gcc
    goto :fail
)
where gcc
REM --- ninja ---------------------------------------------------------------
set "NINJA_EXE="
for /f "delims=" %%N in ('where ninja 2^>nul') do if not defined NINJA_EXE set "NINJA_EXE=%%N"
for %%N in (
    "C:\msys64\ucrt64\bin\ninja.exe"
    "C:\msys64\mingw64\bin\ninja.exe"
    "C:\msys64\mingw32\bin\ninja.exe"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
) do (
    if not defined NINJA_EXE if exist %%N set "NINJA_EXE=%%~N"
)
if not defined NINJA_EXE (
    echo ERROR: no ninja on PATH and none found under C:\msys64 or Visual Studio.
    echo        Install it with: pacman -S mingw-w64-ucrt-x86_64-ninja
    goto :fail
)
echo Using ninja: !NINJA_EXE!

REM Bare includes (Minecraft.h et al) live in src/net/minecraft/src. The
REM console toolchains add that path themselves; on desktop every CI job
REM injects it via CMAKE_CXX_FLAGS, so this wrapper does the same -- without
REM it the game sources fail with "No such file or directory". The preset's
REM own -m32 -msse2 -mfpmath=sse must be repeated here: a command-line -D
REM replaces the preset value instead of appending to it.
"!CMAKE_EXE!" --preset gcc32-release -DCMAKE_MAKE_PROGRAM="!NINJA_EXE!" "-DCMAKE_CXX_FLAGS=-m32 -msse2 -mfpmath=sse -I%CD:\=/%/src/net/minecraft/src"
if errorlevel 1 goto :fail

"!CMAKE_EXE!" --build --preset gcc32-release --parallel
if errorlevel 1 goto :fail

pause
exit /b 0

:fail
echo.
echo BUILD FAILED
pause
exit /b 1
