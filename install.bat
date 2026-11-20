@echo off
setlocal EnableExtensions
rem Run as Administrator
set "CONFIG=Release"
if /i "%~1"=="debug" set "CONFIG=Debug"
set "SRC=%~dp0dist\Oto Sampler.vst3"
if not exist "%SRC%" set "SRC=%SystemDrive%\oto_build\OtoSampler_artefacts\%CONFIG%\VST3\Oto Sampler.vst3"
set "DST=%CommonProgramFiles%\VST3"

if not exist "%SRC%" (
    echo Build first with build.bat
    if not defined OTO_NO_PAUSE pause
    exit /b 1
)

xcopy "%SRC%" "%DST%\Oto Sampler.vst3\" /E /I /Y
if errorlevel 1 (
    echo Copy failed. Run this file as Administrator.
    if not defined OTO_NO_PAUSE pause
    exit /b 1
)
echo Installed to "%DST%"
if not defined OTO_NO_PAUSE pause
exit /b 0
