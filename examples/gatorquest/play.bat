@echo off
rem Gator Quest launcher: finds jaiscript.exe in the build tree (build\bin) or beside this file (the shipped demo).
setlocal
cd /d "%~dp0"
set "EXE=%~dp0..\..\build\bin\jaiscript.exe"
if not exist "%EXE%" set "EXE=%~dp0jaiscript.exe"
if not exist "%EXE%" (
	echo Could not find jaiscript.exe. Build the runner from the JaiScript source root:
	echo   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	echo   cmake --build build --config Release --target jaiscript_runner
	echo or copy jaiscript.exe next to play.bat.
	pause
	exit /b 1
)
"%EXE%" --no-pause "%~dp0scripts\gatorquest.jai" -- %*
