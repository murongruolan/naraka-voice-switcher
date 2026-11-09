@echo off
rem Naraka Voice Tool build script (MSVC 2022 BuildTools, manual env)
rem NOTE: no vcvars64 (it shells out to reg.exe); set INCLUDE/LIB directly.
setlocal
set "MSVC=F:\VS2022BuildTools\VC\Tools\MSVC\14.44.35207"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"

if not exist "%MSVC%\bin\Hostx64\x64\cl.exe" (
    echo [ERROR] cl.exe not found: %MSVC%
    exit /b 1
)

set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"

if not exist build mkdir build
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /I tools src\main.cpp src\akpk.cpp src\roles.cpp src\rebuild.cpp src\gui.cpp /Fe:build\NarakaVoiceTool.exe
if errorlevel 1 (
    echo [ERROR] compile failed
    exit /b 1
)
echo.
echo BUILD OK: build\NarakaVoiceTool.exe
