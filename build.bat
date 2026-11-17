@echo off
setlocal
cd /d "%~dp0"

rem Usage:  build.bat           configure + build Release
rem         build.bat clean     delete the build folder first
rem         build.bat debug     build Debug instead of Release

set CONFIG=Release
set PRESET=release
set CLEAN=0
:args
if "%~1"=="" goto :argsdone
if /i "%~1"=="clean" set CLEAN=1
if /i "%~1"=="debug" ( set CONFIG=Debug & set PRESET=debug )
shift
goto :args
:argsdone

where cmake >nul 2>&1
if errorlevel 1 ( echo CMake was not found in PATH. & goto :err )
where git >nul 2>&1
if errorlevel 1 ( echo Git was not found in PATH. JUCE is fetched with Git. & goto :err )

if "%CLEAN%"=="1" if exist build rmdir /s /q build

set CFG_PRESET=vs2026
cmake --help | findstr /C:"Visual Studio 18 2026" >nul 2>&1
if errorlevel 1 (
    echo Generator "Visual Studio 18 2026" was not found in this CMake. Falling back to Visual Studio 2022.
    set CFG_PRESET=vs2022
    if /i "%CONFIG%"=="Release" set PRESET=release-vs2022
)

echo [1/2] Configure with preset %CFG_PRESET% (JUCE is fetched with Git on first run)
cmake --preset %CFG_PRESET%
if errorlevel 1 goto :err

echo [2/2] Build %CONFIG%
cmake --build --preset %PRESET% --parallel
if errorlevel 1 goto :err

echo.
echo === Done ===
echo VST3       : %~dp0build\OtoSampler_artefacts\%CONFIG%\VST3\Oto Sampler.vst3
echo Standalone : %~dp0build\OtoSampler_artefacts\%CONFIG%\Standalone\Oto Sampler.exe
echo Run install.bat as Administrator to copy the VST3 to the system VST3 folder.
if not defined OTO_NO_PAUSE pause
exit /b 0

:err
echo.
echo Build failed.
if not defined OTO_NO_PAUSE pause
exit /b 1
