@echo off
cd /d "%~dp0"
del "mcrelease.zip"
7z a mcrelease.zip "src" "cmake" "CMakeLists.txt" "CMakePresets.json" "AGENTS.md"