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
    if exist "build\gcc-debug" rmdir /s /q "build\gcc-debug"
)

REM --- toolchain: gcc + ninja ----------------------------------------------
REM The gcc-* presets use gcc/g++ from PATH with the Ninja generator, and this
REM machine keeps neither on PATH (only devkitPro's MSYS2 cmake is on PATH).
REM The system MSYS2 at C:\msys64 ships a UCRT64 gcc + ninja under
REM C:\msys64\ucrt64\bin, so put that dir on PATH for this session when gcc
REM is missing. Ninja is passed explicitly via -DCMAKE_MAKE_PROGRAM: the
REM presets no longer pin it, and without it the Ninja generator fails before
REM the compiler check even runs.
where gcc >nul 2>nul
if not errorlevel 1 goto :haveToolchain
set "TOOLBIN="
for %%T in (
    "C:\msys64\ucrt64\bin"
    "C:\msys64\mingw64\bin"
) do (
    if not defined TOOLBIN if exist "%%~T\gcc.exe" set "TOOLBIN=%%~T"
)
if not defined TOOLBIN (
    echo ERROR: no gcc on PATH and none found under C:\msys64 ^(ucrt64/mingw64^).
    echo        Install it with: pacman -S mingw-w64-ucrt-x86_64-gcc
    goto :fail
)
set "PATH=!TOOLBIN!;!PATH!"
echo Using gcc from: !TOOLBIN!
:haveToolchain
where gcc
REM --- ninja ---------------------------------------------------------------
set "NINJA_EXE="
for /f "delims=" %%N in ('where ninja 2^>nul') do if not defined NINJA_EXE set "NINJA_EXE=%%N"
for %%N in (
    "C:\msys64\ucrt64\bin\ninja.exe"
    "C:\msys64\mingw64\bin\ninja.exe"
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
REM it the game sources fail with "No such file or directory".
"!CMAKE_EXE!" --preset gcc-debug -DCMAKE_MAKE_PROGRAM="!NINJA_EXE!" "-DCMAKE_CXX_FLAGS=-I%CD:\=/%/src/net/minecraft/src"
if errorlevel 1 goto :fail

"!CMAKE_EXE!" --build --preset gcc-debug --parallel
if errorlevel 1 goto :fail

pause
exit /b 0

:fail
echo.
echo BUILD FAILED
pause
exit /b 1
