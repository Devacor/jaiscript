@echo off
rem JaiDOOM launcher: double-click me. Works from any copied location, using the
rem jaiscript.exe sitting next to this file (or one on PATH); the game finds wads/ from
rem its own location, so dropping jaidoom.jai onto jaiscript.exe works as well.
cd /d "%~dp0scripts"
if exist "%~dp0jaiscript.exe" (
	"%~dp0jaiscript.exe" jaidoom.jai
) else (
	jaiscript.exe jaidoom.jai
)
