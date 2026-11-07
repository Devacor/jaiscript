@echo off
rem Removes everything bootstrap.bat fetched or generated: the WADs, the download folder, the
rem audio cache and any stray .wav/.mid the game wrote next to its scripts, plus the per-user
rem data folder the game uses when this folder is read-only. Keeps COPYING.txt.
setlocal EnableDelayedExpansion
cd /d "%~dp0"
echo JaiDOOM: removing downloaded game data and generated caches
set "REMOVED=0"
for %%F in (wads\doom1.wad wads\freedoom1.wad wads\freedoom2.wad wads\SOURCES.txt) do (
	if exist "%%F" (del /q "%%F" & echo    removed  %%F & set /a REMOVED+=1)
)
if exist wads\.download (rmdir /s /q wads\.download & echo    removed  wads\.download\ & set /a REMOVED+=1)
if exist cache (rmdir /s /q cache & echo    removed  cache\ & set /a REMOVED+=1)
if exist "%LOCALAPPDATA%\JaiDOOM" (rmdir /s /q "%LOCALAPPDATA%\JaiDOOM" & echo    removed  %LOCALAPPDATA%\JaiDOOM\ & set /a REMOVED+=1)
for %%F in (*.wav *.mid scripts\*.wav scripts\*.mid) do (
	if exist "%%F" (del /q "%%F" & echo    removed  %%F & set /a REMOVED+=1)
)
if "%REMOVED%"=="0" (echo    nothing to remove) else (echo    done ^(%REMOVED% items^). Run bootstrap.bat to fetch everything again.)
exit /b 0
