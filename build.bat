@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Usage:  build.bat                 configure + build Release
rem         build.bat clean           delete the build folder first
rem         build.bat debug           build Debug instead of Release
rem         build.bat juce=8.0.8      use another JUCE tag
rem
rem The build runs in a SHORT folder in %SystemDrive%\oto_build\HASH, where HASH is made from the path of this folder. JUCE builds its VST3 helper in a very deep folder,
rem and long paths stop the build right after "juce_VST3ManifestHelper.cpp".
rem Results are copied to the "dist" folder next to this file.
rem If the build fails, build_log.txt is written and the error lines are shown.

set "CONFIG=Release"
set "CLEAN=0"
set "JUCE_TAG="
set "STEP="
:args
if "%~1"=="" goto :argsdone
set "ARG=%~1"
if /i "%ARG%"=="clean" set "CLEAN=1"
if /i "%ARG%"=="debug" set "CONFIG=Debug"
if /i "%ARG:~0,5%"=="juce=" set "JUCE_TAG=%ARG:~5%"
shift
goto :args
:argsdone

rem One build folder per project location (hash of this folder path), so several copies never share a CMake cache.
set "OTOHASH="
for /f %%H in ('powershell -NoProfile -Command "[BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes('%~dp0'.ToLower()))).Replace('-','').Substring(0,8)"') do set "OTOHASH=%%H"
if not defined OTOHASH set "OTOHASH=default"
set "BDIR=%SystemDrive%\oto_build\%OTOHASH%"
set "LOG=%~dp0build_log.txt"
set "DIST=%~dp0dist"
set "ART=%BDIR%\OtoSampler_artefacts\%CONFIG%"

where cmake >nul 2>&1
if errorlevel 1 ( echo CMake was not found in PATH. & goto :fail )
where git >nul 2>&1
if errorlevel 1 ( echo Git was not found in PATH. JUCE is fetched with Git. & goto :fail )

echo CMake version:
cmake --version | findstr /B /C:"cmake version"

set "GEN=Visual Studio 18 2026"
cmake --help | findstr /C:"Visual Studio 18 2026" >nul 2>&1
if errorlevel 1 (
    echo Generator "Visual Studio 18 2026" was not found in this CMake. Falling back to Visual Studio 2022.
    set "GEN=Visual Studio 17 2022"
)

if "%CLEAN%"=="1" if exist "%BDIR%" rmdir /s /q "%BDIR%"

if exist "%BDIR%\CMakeCache.txt" (
    findstr /C:"CMAKE_GENERATOR:INTERNAL=%GEN%" "%BDIR%\CMakeCache.txt" >nul 2>&1
    if errorlevel 1 (
        echo The generator changed. Removing the old build folder.
        rmdir /s /q "%BDIR%"
    )
)

set "JUCE_OPT="
if defined JUCE_TAG set "JUCE_OPT=-DOTO_JUCE_TAG=%JUCE_TAG%"

echo.
echo [1/2] Configure: %GEN%  (build folder: %BDIR%)
echo       JUCE is downloaded with Git on the first run. This can take a few minutes.
set "STEP=configure"
cmake -S "%~dp0." -B "%BDIR%" -G "%GEN%" -A x64 %JUCE_OPT%
if errorlevel 1 goto :fail

echo.
echo [2/2] Build: %CONFIG%
set "STEP=build"
cmake --build "%BDIR%" --config %CONFIG% --parallel
if errorlevel 1 goto :fail

if not exist "%ART%\VST3\Oto Sampler.vst3" (
    echo The build finished but the VST3 was not found in "%ART%\VST3".
    set "STEP=missing"
    goto :fail
)

if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%"
xcopy "%ART%\VST3\Oto Sampler.vst3" "%DIST%\Oto Sampler.vst3\" /E /I /Y /Q >nul
if exist "%ART%\Standalone\Oto Sampler.exe" copy /Y "%ART%\Standalone\Oto Sampler.exe" "%DIST%\" >nul

echo.
echo === Done ===
echo VST3       : %DIST%\Oto Sampler.vst3
echo Standalone : %DIST%\Oto Sampler.exe
echo Run install.bat as Administrator to copy the VST3 to the system VST3 folder.
if not defined OTO_NO_PAUSE pause
exit /b 0

:fail
echo.
echo ===== Build failed =====
if "%STEP%"=="configure" cmake -S "%~dp0." -B "%BDIR%" -G "%GEN%" -A x64 %JUCE_OPT% > "%LOG%" 2>&1
if "%STEP%"=="build" cmake --build "%BDIR%" --config %CONFIG% > "%LOG%" 2>&1
if exist "%LOG%" (
    echo Full log: %LOG%
    echo ---- error lines ----
    findstr /i /c:" error " /c:"fatal error" /c:"CMake Error" /c:"LNK" "%LOG%"
)
if not defined OTO_NO_PAUSE pause
exit /b 1
