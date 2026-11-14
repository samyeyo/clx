# Run the official Lua 5.5.1 testsuite (this directory) through clx on Windows.
# run.bat is a thin wrapper for this file; run.sh is the macOS/Linux counterpart.
# Every test is compiled by clx (harness.lua emits a source file per test); only the
# chunks a test creates at run time with load/loadfile go through the embedded VM.

$ErrorActionPreference = 'Stop'

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent (Split-Path -Parent $Here)

$All = $false
$Extra = $false
$ListOnly = $false
$ShowErrors = $false
$Help = $false
$Tests = @()

foreach ($a in $args) {
    switch ($a) {
        '--all' { $All = $true }
        '--extra' { $Extra = $true }
        '--list' { $ListOnly = $true }
        '-v' { $ShowErrors = $true }
        '--verbose' { $ShowErrors = $true }
        '-h' { $Help = $true }
        '--help' { $Help = $true }
        default {
            if ($a.StartsWith('-')) {
                Write-Host "unknown option: $a" -ForegroundColor Red
                exit 2
            }
            $Tests += $a
        }
    }
}

if ($Help) {
    @"
usage: run.bat [options] [test.lua ...]

Compiles the vendored Lua 5.5 test suite (this directory) with clx --dynamic and runs it.
With no file arguments every test file is compiled and run, one process each.

options:
  --all          run the suite the upstream way: one process, all.lua order
  --extra        also run bwcoercion.lua and heavy.lua, which upstream keeps out of all.lua
  --list         list the test files instead of running them
  -v|--verbose   print the failing assertion for each failed test
  -h|--help      this message

environment:
  CLX=<path>     clx compiler (default: the repo's build tree)
  TIMEOUT=<sec>  per-test timeout (default: 180)
  SOFT=1         set _soft, skipping the long and memory hungry tests
  PORT=1         set _port, skipping the non-portable tests
  BUILD=<dir>    work + logs directory (default: %TEMP%\clx-lua55)
"@
    exit 0
}

#------------------ the compiler: $env:CLX wins, then the usual build locations
function Find-Clx {
    if ($env:CLX) { return $env:CLX }
    $candidates = @(
        (Join-Path $Root 'build\bin\Release\clx.exe'),
        (Join-Path $Root 'build\bin\clx.exe'),
        (Join-Path $Root 'build\clx.exe'),
        (Join-Path $Root 'bin\clx.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

$Clx = Find-Clx
if (-not $Clx -or -not (Test-Path $Clx)) {
    Write-Host "clx.exe not found (build it first, or set CLX=...)" -ForegroundColor Red
    exit 1
}

$BuildDir = if ($env:BUILD) { $env:BUILD } elseif ($env:TEMP) { $env:TEMP } else { '.' }
$BuildDir = Join-Path $BuildDir 'clx-lua55'
$OutDir = Join-Path $BuildDir 'out'
$BinDir = Join-Path $OutDir 'bin'
$Work = Join-Path $BuildDir 'suite'
$Timeout = if ($env:TIMEOUT) { [int]$env:TIMEOUT } else { 180 }

if ($env:SOFT -eq '1') { $env:CLX_LUA55_SOFT = '1' }
if ($env:PORT -eq '1') { $env:CLX_LUA55_PORT = '1' }

Write-Host "Lua 5.5 suite via: $Clx --dynamic (compiled, one binary per test)"

New-Item -ItemType Directory -Force -Path $BinDir | Out-Null

#------------------ the tests write files next to themselves (libs\A.lua, time.txt, ...), so they
#------------------ run from a scratch copy and the vendored suite stays pristine
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
Copy-Item -Path (Join-Path $Here '*') -Destination $Work -Recurse -Force
foreach ($f in 'run.sh', 'run.bat', 'run.ps1', 'harness.lua') {
    $p = Join-Path $Work $f
    if (Test-Path $p) { Remove-Item -Force $p }
}

#------------------ compile the generator once; it writes "<prelude + test body>" per test
function Find-Exe([string]$base) {
    foreach ($candidate in @($base, "$base.exe")) {
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

$GenBase = Join-Path $BuildDir 'harness'
foreach ($old in @($GenBase, "$GenBase.exe")) {
    if (Test-Path $old) { Remove-Item -Force $old }
}
& $Clx (Join-Path $Here 'harness.lua') '--dynamic' '-o' $GenBase | Out-Null
$Gen = Find-Exe $GenBase
if (-not $Gen) {
    Write-Host "harness build failed" -ForegroundColor Red
    exit 1
}

#------------------ emit one test's translation unit and compile it with clx
function Build-Test([string]$name) {
    $stem = $name -replace '\.lua$', ''
    $src = Join-Path $OutDir ("driver_$stem.lua")
    $bin = Join-Path $BinDir $stem
    Push-Location $Work
    try {
        & $Gen '--emit' $name $src | Out-Null
    } finally {
        Pop-Location
    }
    foreach ($old in @($bin, "$bin.exe")) {
        if (Test-Path $old) { Remove-Item -Force $old }
    }
    & $Clx $src '--dynamic' '-o' $bin 2>&1 | Out-File -FilePath (Join-Path $OutDir "$stem.compile.log") -Encoding utf8
    return (Find-Exe $bin)
}

function Build-All {
    $src = Join-Path $OutDir 'driver_all.lua'
    $bin = Join-Path $BinDir 'all'
    Push-Location $Work
    try {
        & $Gen '--emit-all' $src | Out-Null
    } finally {
        Pop-Location
    }
    foreach ($old in @($bin, "$bin.exe")) {
        if (Test-Path $old) { Remove-Item -Force $old }
    }
    & $Clx $src '--dynamic' '-o' $bin 2>&1 | Out-File -FilePath (Join-Path $OutDir 'all.compile.log') -Encoding utf8
    return (Find-Exe $bin)
}

#------------------ stdin is an empty pipe, never the terminal or a file: tests that seek stdin
#------------------ must get a non-seekable stream (stock lua's own suite runs from a tty for the
#------------------ same reason). A redirected file would be seekable and change the result.
function Invoke-Program {
    param([string]$Exe, [string[]]$Arguments, [string]$Log)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = (($Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ')
    $psi.WorkingDirectory = $Work
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $p.StandardInput.Close()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    $finished = $p.WaitForExit($Timeout * 1000)
    if (-not $finished) {
        try { $p.Kill() } catch { }
        try { [void]$p.WaitForExit(5000) } catch { }
    }
    $out = ''
    $err = ''
    try { $out = $outTask.Result } catch { }
    try { $err = $errTask.Result } catch { }
    Set-Content -Path $Log -Value (($out + $err) + "`n") -NoNewline
    if (-not $finished) {
        Add-Content -Path $Log -Value "TIMEOUT after $Timeout s"
        return 'TIMEOUT'
    }
    $code = $p.ExitCode
    if ($code -eq 0) { return 'PASS' }
    Add-Content -Path $Log -Value "exit code: $code"
    return 'FAIL'
}

function Get-SuiteFiles {
    Push-Location $Work
    try {
        $listArgs = @('--list')
        if ($Extra) { $listArgs += '--extra' }
        return @(& $Gen @listArgs)
    } finally {
        Pop-Location
    }
}

function Show-Failure($log) {
    $err = Select-String -Path $log -Pattern 'assertion failed|cannot open|Error|error:|stack traceback|bad argument' |
        Select-Object -First 1
    if ($err) { Write-Host ("        " + $err.Line) }
}

if ($ListOnly) {
    Get-SuiteFiles
    exit 0
}

if ($All) {
    $log = Join-Path $OutDir 'all.log'
    $exe = Build-All
    if (-not $exe) {
        Write-Host 'COMPILE all'
        Get-Content (Join-Path $OutDir 'all.compile.log') -Tail 5
        exit 1
    }
    Write-Host "single process, upstream order (main.lua skipped)"
    $status = Invoke-Program -Exe $exe -Arguments @() -Log $log
    Write-Host $status
    if ($status -eq 'PASS') { exit 0 }
    Show-Failure $log
    Write-Host "  log: $log"
    exit 1
}

if ($Tests.Count -eq 0) { $Tests = @(Get-SuiteFiles) }

$Pass = 0
$Fail = 0
$failed = @()
foreach ($name in $Tests) {
    if (-not (Test-Path (Join-Path $Work $name))) {
        Write-Host "missing: $name" -ForegroundColor Red
        $Fail++
        continue
    }
    $stem = $name -replace '\.lua$', ''
    $log = Join-Path $OutDir "$stem.log"
    $exe = Build-Test $name
    if (-not $exe) {
        Write-Host "COMPILE $name"
        $Fail++
        $failed += $name
        if ($ShowErrors) {
            Get-Content (Join-Path $OutDir "$stem.compile.log") -Tail 1 | ForEach-Object { Write-Host ("        " + $_) }
        }
        continue
    }
    $status = Invoke-Program -Exe $exe -Arguments @() -Log $log
    Write-Host ('{0,-7} {1}' -f $status, $name)
    if ($status -eq 'PASS') {
        $Pass++
    } else {
        $Fail++
        $failed += $name
        if ($ShowErrors) { Show-Failure $log }
    }
}

Write-Host '-----'
Write-Host "pass: $Pass  fail: $Fail   (logs in $OutDir)"
if ($Fail -gt 0) {
    Write-Host ("failed: " + ($failed -join ' '))
    exit 1
}
exit 0
