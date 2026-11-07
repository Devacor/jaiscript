@echo off
rem JaiScript installer for Windows CMD: runs install.ps1 (https://jaiscript.com/install.ps1) in Windows PowerShell.
rem   curl -fsSL https://jaiscript.com/install.cmd -o install.cmd && install.cmd && del install.cmd
rem Its options work here too (install.cmd --no-modify-path); JAISCRIPT_INSTALL_PS1 names a local copy to run instead.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12; $script = if ($env:JAISCRIPT_INSTALL_PS1) { Get-Content -Raw -LiteralPath $env:JAISCRIPT_INSTALL_PS1 } else { Invoke-RestMethod https://jaiscript.com/install.ps1 }; try { & ([scriptblock]::Create($script)) %* } catch { if (-not $_.Exception.Message.StartsWith('jaiscript-install:')) { Write-Host $_ -ForegroundColor Red }; exit 1 }"
exit /b %errorlevel%
