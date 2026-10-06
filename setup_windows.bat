@echo off
REM setup_windows.bat — Generate the Visual Studio 2022 solution for babelstreamer-filter.
REM
REM Usage:
REM   setup_windows.bat                        (whisper at %USERPROFILE%\Projects\whisper)
REM   setup_windows.bat D:\Projects\whisper    (custom whisper path)
REM
REM Requirements:
REM   Visual Studio 2022 with "Desktop development with C++" workload
REM   CMake 3.28+  (bundled with VS, or https://cmake.org/download/)
REM   whisper.cpp built (no install required).
REM   The repo root must contain:
REM     include\whisper.h
REM     ggml\include\ggml.h
REM     build\Release\whisper.lib  (and Debug\, RelWithDebInfo\ variants)
REM
REM Output: build_x64\babelstreamer-filter.sln

setlocal

cd /d "%~dp0"

REM Determine whisper root — first argument wins, then %USERPROFILE%\Projects\whisper
if not "%~1"=="" (
    set WHISPER_ROOT=%~1
) else (
    set WHISPER_ROOT=Y:\whisper.cpp-master
)

echo =^> Using whisper root: %WHISPER_ROOT%

if not exist "%WHISPER_ROOT%\include\whisper.h" (
    echo.
    echo ERROR: whisper.h not found under %WHISPER_ROOT%\include\
    echo        Make sure WHISPER_ROOT points to the whisper.cpp repo root, e.g.:
    echo          setup_windows.bat D:\Projects\whisper
    pause
    exit /b 1
)

if not exist "%WHISPER_ROOT%\Windows\src\Release\whisper.lib" (
    echo.
    echo WARNING: %WHISPER_ROOT%\Windows\src\Release\whisper.lib not found.
    echo          Build whisper.cpp first:
    echo            cd %WHISPER_ROOT%
    echo            cmake -B Windows -A x64 -DBUILD_SHARED_LIBS=OFF -DGGML_BLAS=OFF -DGGML_VULKAN=ON
    echo            cmake --build Windows --config Release
    echo            cmake --build Windows --config Debug
    echo.
    echo          To add NVIDIA acceleration instead of/alongside Vulkan, use
    echo          -DGGML_CUDA=ON and pass -DWHISPER_HAS_CUDA=1 to this script's
    echo          cmake --preset step below. For AMD GPUs use -DGGML_HIP=ON and
    echo          -DWHISPER_HAS_HIP=1 instead. GGML_CUDA and GGML_HIP are mutually
    echo          exclusive in a single build ^(they export the same symbols^);
    echo          GGML_VULKAN can be combined with either as the runtime fallback.
    echo.
)

echo =^> Configuring babelstreamer-filter (Windows x64, Visual Studio 2022)
cmake --preset windows-x64 -DWHISPER_ROOT="%WHISPER_ROOT%"
if %ERRORLEVEL% neq 0 (
    echo.
    echo ERROR: CMake configuration failed. See output above.
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo =^> Done. Opening Visual Studio solution...
start "" "build_x64\babelstreamer-filter.sln"
