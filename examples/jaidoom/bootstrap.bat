@echo off
rem JaiDOOM bootstrap, headless: the game fetches Freedoom and the DOOM shareware episode itself
rem (boot.jai), verifies them by SHA-256 and decodes the audio. Same as the first-run screen.
rem   bootstrap.bat [--no-shareware] [--no-audio]
rem Needs jaiscript.exe: JAI_RUNNER, one next to this file, one on PATH, or the repo build.
setlocal
cd /d "%~dp0scripts"
set "RUNNER="
if defined JAI_RUNNER if exist "%JAI_RUNNER%" set "RUNNER=%JAI_RUNNER%"
if not defined RUNNER if exist "%~dp0jaiscript.exe" set "RUNNER=%~dp0jaiscript.exe"
if not defined RUNNER if exist "%~dp0..\..\build\bin\jaiscript.exe" set "RUNNER=%~dp0..\..\build\bin\jaiscript.exe"
if not defined RUNNER for %%P in (jaiscript.exe) do if not "%%~$PATH:P"=="" set "RUNNER=%%~$PATH:P"
if not defined RUNNER (
	echo jaiscript.exe not found: set JAI_RUNNER, put it next to this script, or build it ^(docs\BUILD.md^)
	exit /b 1
)
"%RUNNER%" --no-pause jaidoom.jai -- bootstrap %*
exit /b %ERRORLEVEL%
