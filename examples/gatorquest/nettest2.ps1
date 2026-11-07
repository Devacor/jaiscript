# Gator Quest two-process netplay test: a dedicated host and two scripted clients,
# three separate jaiscript processes talking over loopback.
#   powershell -File nettest2.ps1 [-Exe path\to\jaiscript.exe] [-Backend vm|interp] [-Port 28788]
param([string]$Exe = "", [string]$Backend = "vm", [int]$Port = 28788)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Exe -eq "") { $Exe = Join-Path $here "jaiscript.exe" }
if (-not (Test-Path $Exe)) { $Exe = Join-Path $here "..\..\out\build\x64-Release BENCHMARKS\bin\jaiscript.exe" }
$game = Join-Path $here "scripts\gatorquest.jai"
$tmp = Join-Path $env:TEMP "gatorquest_nettest2_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$common = @("--no-pause", "--backend=$Backend", $game, "--")
$host_p = Start-Process -FilePath $Exe -ArgumentList ($common + @("serve", "port", "$Port", "ticks", "150", "nosave", "seed", "4")) -RedirectStandardOutput "$tmp\host.txt" -RedirectStandardError "$tmp\host_err.txt" -PassThru -NoNewWindow
Start-Sleep -Milliseconds 1500
$a = Start-Process -FilePath $Exe -ArgumentList ($common + @("client", "127.0.0.1", "port", "$Port", "name", "Alpha", "frames", "200", "say", '"hello from alpha"', "walk", "2")) -RedirectStandardOutput "$tmp\a.txt" -RedirectStandardError "$tmp\a_err.txt" -PassThru -NoNewWindow
$b = Start-Process -FilePath $Exe -ArgumentList ($common + @("client", "127.0.0.1", "port", "$Port", "name", "Bravo", "frames", "200", "say", '"bravo says hi"', "walk", "6")) -RedirectStandardOutput "$tmp\b.txt" -RedirectStandardError "$tmp\b_err.txt" -PassThru -NoNewWindow
$a.WaitForExit(120000) | Out-Null
$b.WaitForExit(120000) | Out-Null
$host_p.WaitForExit(120000) | Out-Null
foreach ($f in @("host", "a", "b")) {
	Get-Content "$tmp\$f.txt"
	$err = Get-Content "$tmp\${f}_err.txt" -ErrorAction SilentlyContinue
	if ($err) { $err }
}
$all = (Get-Content "$tmp\a.txt", "$tmp\b.txt", "$tmp\host.txt") -join "`n"
$checks = @(
	@("Alpha joined", "client Alpha: joined as slot"),
	@("Bravo joined", "client Bravo: joined as slot"),
	@("Alpha saw Bravo", "client Alpha: .*saw other players: .*Bravo"),
	@("Bravo saw Alpha", "client Bravo: .*saw other players: .*Alpha"),
	@("Alpha heard Bravo", 'client Alpha: heard "Bravo: bravo says hi"'),
	@("Bravo heard Alpha", 'client Bravo: heard "Alpha: hello from alpha"'),
	@("host saw two players", "serve: .*peak 2 players, 2 joins, 2 leaves")
)
$fails = 0
foreach ($c in $checks) {
	if ($all -match $c[1]) { "  ok    " + $c[0] } else { "  FAIL  " + $c[0]; $fails++ }
}
Remove-Item -Recurse -Force $tmp
"nettest2 ($Backend): $($checks.Count - $fails) passed, $fails failed"
exit $fails
