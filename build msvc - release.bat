@echo off
REM Windows x64 MSVC build. Mirrors the build-windows-x64-msvc CI job:
REM   cmake -B build/msvc-release -G "Visual Studio 18 2026" -A x64 -DMC_LOG_LEVEL=2
REM   cmake --build build/msvc-release --config Release --parallel
REM
REM This script deliberately calls a NATIVE WINDOWS cmake by full path instead of
REM whatever `cmake` resolves to. devkitPro's installer puts its MSYS2 bin
REM directory on the system PATH ahead of everything else, so a bare `cmake` is
REM the MSYS2 build even from cmd.exe. For the Visual Studio generator any cmake
REM works, but stick to native for consistency with the other build *.bat files.
REM
REM Usage:
REM   build msvc - release.bat         configure + build Release with MSVC x64
REM   build msvc - release.bat clean   wipe build\msvc-release first
setlocal enabledelayedexpansion
cd /d "%~dp0"

if /I "%~1"=="clean" (
    if exist "build\msvc-release" rmdir /s /q "build\msvc-release"
)

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
    set "CMAKE_EXE=cmake"
)
echo Using cmake: !CMAKE_EXE!

REM Bare includes (Minecraft.h et al) live in src/net/minecraft/src; the
REM console toolchains add that path themselves, desktop needs it from here.
set "CXXFLAGS=%CXXFLAGS% -I%CD:\=/%/src/net/minecraft/src"

REM Prefer the newest generator CI uses, fall back to VS 2022 (a stale cache
REM from the failed attempt must go: it pins the generator that just failed).
set "GEN=Visual Studio 18 2026"
"!CMAKE_EXE!" -B build/msvc-release -G "!GEN!" -A x64 -DMC_LOG_LEVEL=2
if errorlevel 1 (
    echo Generator "!GEN!" failed, retrying with Visual Studio 17 2022 ...
    if exist "build\msvc-release" rmdir /s /q "build\msvc-release"
    set "GEN=Visual Studio 17 2022"
    "!CMAKE_EXE!" -B build/msvc-release -G "!GEN!" -A x64 -DMC_LOG_LEVEL=2
)
if errorlevel 1 goto :fail

"!CMAKE_EXE!" --build build/msvc-release --config Release --parallel
if errorlevel 1 goto :fail

echo.
echo Done: bin\Release\OptiCraft.exe
pause
exit /b 0

:fail
echo.
echo BUILD FAILED
pause
exit /b 1
