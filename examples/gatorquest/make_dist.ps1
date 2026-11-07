# Builds the shippable Gator Quest folder: dist\GatorQuest\ (gitignored).
#   powershell -File make_dist.ps1 [-Exe path\to\jaiscript.exe] [-Out path\to\dist]
# The runner is statically linked (only Windows system DLLs), so the exe, the scripts and the
# launchers are the whole game. The audio cache is synthesized here so the first launch is instant,
# then the folder is checked with a short headless smoke run by the copied exe.
param([string]$Exe = "", [string]$Out = "")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Exe -eq "") { $Exe = Join-Path $here "..\..\out\build\x64-Release BENCHMARKS\bin\jaiscript.exe" }
if (-not (Test-Path $Exe)) { throw "jaiscript.exe not found at $Exe (build the jaiscript_runner target or pass -Exe)" }
if ($Out -eq "") { $Out = Join-Path $here "dist" }
$game = Join-Path $Out "GatorQuest"
if (Test-Path $game) { Remove-Item -Recurse -Force $game }
New-Item -ItemType Directory -Force (Join-Path $game "scripts") | Out-Null

Copy-Item $Exe (Join-Path $game "jaiscript.exe")
Get-ChildItem (Join-Path $here "scripts") -Filter *.jai | Copy-Item -Destination (Join-Path $game "scripts")
Copy-Item (Join-Path $here "dist_readme.txt") (Join-Path $game "README.txt")

$launch = @'
@echo off
rem Gator Quest: {0}
cd /d "%~dp0"
"%~dp0jaiscript.exe" --no-pause "%~dp0scripts\gatorquest.jai" -- {1} %*
'@
Set-Content -Path (Join-Path $game "play.bat") -Value ($launch -f "the title screen: single player, host a game or join one", "") -Encoding ASCII
Set-Content -Path (Join-Path $game "host.bat") -Value ($launch -f "host a multiplayer game: opens the host page (port 28777, or: host.bat port N)", "host") -Encoding ASCII
$join = @'
@echo off
rem Gator Quest: join a game. Usage: join.bat [host address] [port N] (no address: type it on the join page)
cd /d "%~dp0"
"%~dp0jaiscript.exe" --no-pause "%~dp0scripts\gatorquest.jai" -- join %*
'@
Set-Content -Path (Join-Path $game "join.bat") -Value $join -Encoding ASCII
Set-Content -Path (Join-Path $game "serve.bat") -Value ($launch -f "a dedicated host with no screen (port 28777, or: serve.bat port N)", "serve") -Encoding ASCII

# the audio cache, made by the shipped exe itself
& (Join-Path $game "jaiscript.exe") --no-pause (Join-Path $game "scripts\gatorquest.jai") -- prepare
if ($LASTEXITCODE -ne 0) { throw "prepare failed" }

# a short check that the folder runs on its own
$check = & (Join-Path $game "jaiscript.exe") --no-pause (Join-Path $game "scripts\gatorquest.jai") -- smoke ticks 300 bots 1 noroundtrip 2>&1
$hash = ($check | Select-String "STATE_HASH").ToString()
if ($LASTEXITCODE -ne 0 -or $hash -eq "") { $check; throw "the dist smoke failed" }

# leave no run artifacts behind (parse caches and test folders are made on first use)
Get-ChildItem $game -Recurse -Directory -Force -Filter .cache | Remove-Item -Recurse -Force
if (Test-Path (Join-Path $game "saves")) { Remove-Item -Recurse -Force (Join-Path $game "saves") }

$files = Get-ChildItem $game -Recurse -File
$bytes = ($files | Measure-Object -Property Length -Sum).Sum
"dist: $game"
"      $($files.Count) files, $([math]::Round($bytes / 1MB, 2)) MB ($bytes bytes); smoke $hash"
