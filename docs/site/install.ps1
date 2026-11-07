# JaiScript installer for Windows (PowerShell 5.1 or 7): clones JaiScript, builds the jaiscript runner in Release with
# CMake and Visual Studio, and puts it on your user PATH. Running it again updates the clone and rebuilds.
#
#   irm https://jaiscript.com/install.ps1 | iex
#   & ([scriptblock]::Create((irm https://jaiscript.com/install.ps1))) --no-modify-path
#
# Options, each with its environment variable:
#   --dir <folder>       JAISCRIPT_HOME              the install folder (default %LOCALAPPDATA%\jaiscript): src, build and bin
#   --ref <tag|branch>   JAISCRIPT_REF               what to build (default: the newest release tag, else the default branch)
#   --repo <url>         JAISCRIPT_REPO              the repository to clone
#   --generator <name>   CMAKE_GENERATOR             a CMake generator other than Visual Studio: Ninja (with --cxx)
#   --cxx <compiler>     CXX                         a C++ compiler for that generator: clang-cl, clang++, g++
#   --no-modify-path     JAISCRIPT_NO_MODIFY_PATH=1  leave your user PATH alone
#
# It needs git, CMake 3.25 or newer and a C++20 compiler: Visual Studio 2022 or newer with the C++ workload. It never
# asks for administrator rights and writes only to the install folder and, unless told not to, your user PATH.
& {
	$ErrorActionPreference = 'Stop'

	function Say([string]$text) { Write-Host "jaiscript-install: $text" }
	function Fail([string]$text, [string[]]$fixes = @()) {
		Write-Host "jaiscript-install: error: $text" -ForegroundColor Red
		foreach ($fix in $fixes) { Write-Host "  $fix" }
		throw "jaiscript-install: $text"
	}
	function Assert-ExitCode([string]$what) {
		if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit code $LASTEXITCODE)" }
	}

	$root = if ($env:JAISCRIPT_HOME) { $env:JAISCRIPT_HOME } else { Join-Path $env:LOCALAPPDATA 'jaiscript' }
	$ref = "$env:JAISCRIPT_REF"
	$repo = if ($env:JAISCRIPT_REPO) { $env:JAISCRIPT_REPO } else { 'https://github.com/Devacor/jaiscript.git' }
	$generator = "$env:CMAKE_GENERATOR"
	$cxx = "$env:CXX"
	$modifyPath = -not ($env:JAISCRIPT_NO_MODIFY_PATH -and $env:JAISCRIPT_NO_MODIFY_PATH -ne '0')
	$options = '--dir <folder> --ref <tag|branch> --repo <url> --generator <name> --cxx <compiler> --no-modify-path'
	for ($i = 0; $i -lt $args.Count; $i++) {
		$name = "$($args[$i])"
		if ($name -in '--dir', '--ref', '--repo', '--generator', '--cxx') {
			if ($i + 1 -ge $args.Count) { Fail "$name needs a value" }
			$i++
			$value = "$($args[$i])"
			switch ($name) {
				'--dir' { $root = $value }
				'--ref' { $ref = $value }
				'--repo' { $repo = $value }
				'--generator' { $generator = $value }
				'--cxx' { $cxx = $value }
			}
		} elseif ($name -eq '--no-modify-path') {
			$modifyPath = $false
		} elseif ($name -in '-h', '--help') {
			Say "options: $options"
			return
		} else {
			Fail "unknown option $name" @("options: $options")
		}
	}
	$root = [IO.Path]::GetFullPath($root)

	if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
		Fail 'git is not installed.' @('winget install --id Git.Git -e', 'or https://git-scm.com/download/win')
	}

	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	$vs = $null
	if (Test-Path $vswhere) {
		$found = & $vswhere -latest -products * -version '[17.0,' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
		if ($found) { $vs = @($found)[0] }
	}

	$cmake = Get-Command cmake -ErrorAction SilentlyContinue
	$cmake = if ($cmake) { $cmake.Source } else { $null }
	if (-not $cmake -and $vs) {
		$bundled = Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
		if (Test-Path $bundled) { $cmake = $bundled }
	}
	if (-not $cmake) {
		Fail 'CMake is not installed (3.25 or newer is needed).' @('winget install --id Kitware.CMake -e', 'or https://cmake.org/download/')
	}
	$cmakeHelp = & $cmake --help
	$cmakeVersion = [version]((& $cmake --version | Select-Object -First 1) -replace '^[^0-9]*([0-9]+\.[0-9]+).*$', '$1')
	if ($cmakeVersion -lt [version]'3.25') {
		Fail "CMake $cmakeVersion is too old: 3.25 or newer is needed." @('winget upgrade --id Kitware.CMake -e', 'or https://cmake.org/download/')
	}

	if (-not $generator) {
		if ($vs) {
			$major = ([version]$vs.installationVersion).Major
			$pattern = "Visual Studio $major [0-9]{4}"
			$line = $cmakeHelp | Where-Object { $_ -match "^\*?\s*$pattern\s" } | Select-Object -First 1
			if (-not $line) {
				Fail "CMake $cmakeVersion does not know $($vs.displayName)." @('winget upgrade --id Kitware.CMake -e', 'or https://cmake.org/download/')
			}
			$generator = ([regex]::Match($line, $pattern)).Value
		} else {
			Fail 'no C++ compiler was found: Visual Studio 2022 or newer with the C++ workload is needed.' @(
				'winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"',
				'or install Visual Studio Community with "Desktop development with C++": https://visualstudio.microsoft.com/',
				'or, with LLVM or MinGW and Ninja on PATH: --generator Ninja --cxx clang-cl (or clang++, g++)')
		}
	} elseif ($generator -like 'Ninja*' -and -not (Get-Command ninja -ErrorAction SilentlyContinue)) {
		Fail 'the Ninja generator needs ninja on PATH.' @('winget install --id Ninja-build.Ninja -e')
	}

	if ($generator -like 'Visual Studio*') { $cxx = '' }

	$src = Join-Path $root 'src'
	$build = Join-Path $root 'build'
	$bin = Join-Path $root 'bin'
	New-Item -ItemType Directory -Force -Path $root | Out-Null
	if (-not $ref) {
		$tags = & git ls-remote --tags --refs --sort=-v:refname $repo 'refs/tags/v*'
		Assert-ExitCode "git ls-remote $repo"
		$ref = @($tags | ForEach-Object { if ("$_" -match 'refs/tags/(v[0-9].*)$') { $Matches[1] } }) | Select-Object -First 1
	}
	if (Test-Path (Join-Path $src '.git')) {
		Say "updating $src to $(if ($ref) { $ref } else { 'the default branch' })"
		& git -C $src remote set-url origin $repo; Assert-ExitCode 'git remote set-url'
		& git -C $src fetch --quiet --depth 1 origin $(if ($ref) { $ref } else { 'HEAD' }); Assert-ExitCode 'git fetch'
		& git -C $src checkout --quiet --detach FETCH_HEAD; Assert-ExitCode 'git checkout'
	} elseif (Test-Path $src) {
		Fail "$src exists and is not a git clone." @('remove it, or install elsewhere with --dir <folder>')
	} else {
		Say "cloning $repo$(if ($ref) { " at $ref" }) into $src"
		if ($ref) { & git clone --quiet --depth 1 --branch $ref $repo $src } else { & git clone --quiet --depth 1 $repo $src }
		Assert-ExitCode 'git clone'
	}

	Say "building the runner in $build with $generator and $cmake"
	& $cmake "-DJAI_BUILD_DIR=$build" "-DJAI_BIN_DIR=$bin" -DJAI_RUNNER_ONLY=ON -DJAI_REPLACE_TREE=ON "-DJAI_GENERATOR=$generator" "-DJAI_CXX=$cxx" -P (Join-Path $src 'tools\install\build_runner.cmake')
	Assert-ExitCode 'the build'
	$runner = Join-Path $bin 'jaiscript.exe'
	& $runner --version; Assert-ExitCode 'jaiscript --version'

	$onPath = @($env:Path -split ';' | Where-Object { $_ -and ($_.TrimEnd('\') -ieq $bin.TrimEnd('\')) }).Count -gt 0
	if ($modifyPath) {
		# JAISCRIPT_USER_ENV_KEY lets a test point the PATH change at a scratch registry key
		$key = if ($env:JAISCRIPT_USER_ENV_KEY) { $env:JAISCRIPT_USER_ENV_KEY } else { 'HKCU:\Environment' }
		if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
		$item = Get-Item -Path $key
		$userPath = "$($item.GetValue('Path', '', 'DoNotExpandEnvironmentNames'))"
		$entries = @($userPath -split ';' | Where-Object { $_ })
		$present = @($entries | Where-Object { [Environment]::ExpandEnvironmentVariables($_).TrimEnd('\') -ieq $bin.TrimEnd('\') }).Count -gt 0
		if ($present) {
			Say "your user PATH already has $bin"
		} else {
			$kind = if ($userPath -and $item.GetValueKind('Path') -eq 'String') { 'String' } else { 'ExpandString' }
			Set-ItemProperty -Path $key -Name Path -Value ((@($bin) + $entries) -join ';') -Type $kind
			# setting any user variable through .NET tells running programs (Explorer, new terminals) to reread the environment
			if ($key -eq 'HKCU:\Environment') {
				[Environment]::SetEnvironmentVariable('JAISCRIPT_INSTALL_REFRESH', '1', 'User')
				[Environment]::SetEnvironmentVariable('JAISCRIPT_INSTALL_REFRESH', $null, 'User')
			}
			Say "added $bin to the front of your user PATH ($key)"
		}
		if (-not $onPath) { $env:Path = "$bin;$env:Path" }
		Say 'open a new terminal, then run: jaiscript --version'
	} else {
		Say "your PATH was left alone; to run jaiscript by name, add $bin to your user PATH"
	}
	Say "play Gator Quest: jaiscript `"$(Join-Path $src 'examples\gatorquest\scripts\gatorquest.jai')`""
} @args
