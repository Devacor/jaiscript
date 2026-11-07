# Gator Quest hot-reload test. The test edits the scripts it runs from (a reload is a module
# re-run at its own path), so it runs from a scratch copy of the scripts folder.
#   powershell -File hotreload.ps1 [-Exe path\to\jaiscript.exe] [-Backend vm|interp]
param([string]$Exe = "", [string]$Backend = "vm")
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Exe -eq "") { $Exe = Join-Path $here "..\..\build\bin\jaiscript.exe" }
if (-not (Test-Path $Exe)) { $Exe = Join-Path $here "jaiscript.exe" }
$tmp = Join-Path $env:TEMP "gatorquest_hotreload_$PID"
$scripts = Join-Path $tmp "scripts"
New-Item -ItemType Directory -Force $scripts | Out-Null
New-Item -ItemType Directory -Force (Join-Path $tmp "saves") | Out-Null
Get-ChildItem (Join-Path $here "scripts") -Filter *.jai | Copy-Item -Destination $scripts
Set-Content -Path (Join-Path $scripts "HOTRELOAD_SCRATCH") -Value "a scratch copy: the hot-reload test edits these files" -Encoding ASCII
& $Exe --no-pause "--backend=$Backend" (Join-Path $scripts "gatorquest.jai") -- hotreload
$code = $LASTEXITCODE
Remove-Item -Recurse -Force $tmp
exit $code
