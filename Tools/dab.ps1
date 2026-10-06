# Copyright DreamMoon. All Rights Reserved.
#
# DreamAnim command line.
#
#   dab.ps1 build                 build the test host's editor target
#   dab.ps1 test [-Preset P] [-Filter F] [-NoBuild]
#                                 build, then run the automation tests headless in the test host
#
# Results land in <host>/Saved/DreamAnimTests/<time>-<preset>/: the engine's index.json, the run log
# and summary.md. Exit code: 0 green, 1 red (a test failed or an ensure fired), 2 not judged (the
# build failed, the editor crashed or timed out, or fewer tests ran than the preset expects).

[CmdletBinding()]
param(
    [Parameter(Position = 0)][ValidateSet('build', 'test')][string]$Command = 'test',
    [ValidateSet('Quick', 'All')][string]$Preset = 'Quick',
    [string]$Filter,
    [switch]$NoBuild,
    [string]$Project,
    [int]$TimeoutMinutes = 0
)

$ErrorActionPreference = 'Stop'

$Presets = @{
    Quick = @{ Filter = 'DreamAnim.'; MinTests = 1; Timeout = 30 }
    All   = @{ Filter = 'DreamAnim.'; MinTests = 1; Timeout = 90 }
}

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }
function Write-Note([string]$Text) { Write-Host "    $Text" }
function Write-Problem([string]$Text) { Write-Host "!!  $Text" -ForegroundColor Yellow }

function Find-HostProject {
    if ($Project) { return (Resolve-Path $Project).Path }
    # The plugin normally runs from the host's worktree: <host>/Plugins/DreamAnim/Tools.
    $candidate = Get-ChildItem -Path (Join-Path $PSScriptRoot '..\..\..') -Filter '*.uproject' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -eq 'DreamAnimTestHost.uproject' } | Select-Object -First 1
    if ($candidate) { return $candidate.FullName }
    $default = 'I:\UnrealProject_Moon\DEV_58\DreamAnimTestHost\DreamAnimTestHost.uproject'
    if (Test-Path $default) { return $default }
    throw 'No test host found. Create it with Tools/TestHost/New-DreamAnimTestHost.ps1, or pass -Project.'
}

function Resolve-EngineRoot([string]$UProject) {
    $assoc = (Get-Content -LiteralPath $UProject -Raw | ConvertFrom-Json).EngineAssociation
    $builds = Get-ItemProperty 'HKCU:\Software\Epic Games\Unreal Engine\Builds' -ErrorAction SilentlyContinue
    $root = if ($builds) { $builds.$assoc } else { $null }
    if (-not $root) { throw "Engine association $assoc is not registered." }
    return $root.Replace('/', '\')
}

$uproject = Find-HostProject
$projectDir = Split-Path $uproject
$hostName = [IO.Path]::GetFileNameWithoutExtension($uproject)
$engineRoot = Resolve-EngineRoot $uproject
$editorCmd = Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'

function Invoke-Build {
    Write-Step "Building ${hostName}Editor"
    $log = Join-Path $projectDir 'Saved\DreamAnimTests\build.log'
    New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
    $buildBat = Join-Path $engineRoot 'Engine\Build\BatchFiles\Build.bat'
    & $buildBat "${hostName}Editor" Win64 Development "-Project=$uproject" -WaitMutex -NoHotReloadFromIDE *> $log
    $code = $LASTEXITCODE
    $errors = @(Select-String -LiteralPath $log -Pattern 'error C\d+|error LNK\d+|error :|): error' | Select-Object -First 15 | ForEach-Object { $_.Line.Trim() })
    if ($code -ne 0) {
        Write-Problem "Build failed (exit $code); log: $log"
        foreach ($e in $errors) { Write-Note $e }
        return $false
    }
    Write-Note (Select-String -LiteralPath $log -Pattern 'Result: |Total execution time' | ForEach-Object { $_.Line.Trim() })
    return $true
}

function Get-Digest([string]$RunDir, [int]$Floor, [bool]$TimedOut, $ExitCode, [double]$Seconds) {
    $runLog = Join-Path $RunDir 'run.log'
    $logLines = if (Test-Path -LiteralPath $runLog) { @(Get-Content -LiteralPath $runLog -Encoding utf8) } else { @() }
    $infra = @()
    $crash = @($logLines | Where-Object { $_ -match '=== Critical error: ===|Fatal error!|Unhandled Exception|Assertion failed:' } | Select-Object -First 5)
    if ($crash.Count -gt 0) { $infra += 'The editor crashed:'; $infra += $crash }
    if ($TimedOut) { $infra += 'The run timed out.' }
    $ensures = @($logLines | Where-Object { $_ -match 'Ensure condition failed' } | Select-Object -Unique)
    $loadFailures = @($logLines | Where-Object { $_ -match "Plugin 'DreamAnim' failed to load|Unable to load module.*DreamAnim" } | Select-Object -First 3)
    if ($loadFailures.Count -gt 0) { $infra += 'The plugin did not load:'; $infra += $loadFailures }

    $tests = @()
    $indexPath = Join-Path $RunDir 'index.json'
    if (Test-Path -LiteralPath $indexPath) {
        $index = Get-Content -LiteralPath $indexPath -Raw -Encoding utf8 | ConvertFrom-Json
        foreach ($t in @($index.tests)) {
            $errors = @(); $infos = @()
            foreach ($entry in @($t.entries)) {
                if ($entry.event.type -eq 'Error') { $errors += [string]$entry.event.message }
                elseif ($entry.event.type -eq 'Info') { $infos += [string]$entry.event.message }
            }
            $tests += [pscustomobject]@{ Path = [string]$t.fullTestPath; State = [string]$t.state; Duration = [double]$t.duration; Errors = $errors; Infos = $infos }
        }
    } else {
        $infra += "No index.json in $RunDir; the tests did not report."
    }

    $passed = @($tests | Where-Object { $_.State -eq 'Success' })
    $failed = @($tests | Where-Object { $_.State -eq 'Fail' })
    $ran = $passed.Count + $failed.Count
    if ($ran -lt $Floor) { $infra += "Only $ran test(s) ran; expected at least $Floor. Did DreamAnimTests load?" }

    $code = if ($infra.Count -gt 0) { 2 } elseif ($failed.Count -gt 0 -or $ensures.Count -gt 0) { 1 } else { 0 }
    $word = switch ($code) { 0 { 'green' } 1 { 'red' } default { 'not judged' } }

    $summary = @("# DreamAnim tests -- $word", '')
    $summary += ('Ran {0}: {1} passed, {2} failed; {3:N0} s in the editor.' -f $ran, $passed.Count, $failed.Count, $Seconds)
    if ($infra.Count -gt 0) { $summary += ''; $summary += '## Not judged'; $summary += @($infra | ForEach-Object { "- $_" }) }
    if ($failed.Count -gt 0) {
        $summary += ''; $summary += '## Failed'
        foreach ($t in $failed) { foreach ($e in ($t.Errors | Select-Object -First 5)) { $summary += "- $($t.Path): $e" } }
    }
    if ($ensures.Count -gt 0) { $summary += ''; $summary += '## Ensures'; $summary += @($ensures | ForEach-Object { "- $($_.Trim())" }) }
    $summary += ''; $summary += '## Tests'
    foreach ($t in $tests) {
        $summary += ('- {0} {1} ({2:N0} ms)' -f $t.State, $t.Path, ($t.Duration * 1000.0))
        foreach ($i in ($t.Infos | Select-Object -First 20)) { $summary += "    - $i" }
    }
    Set-Content -LiteralPath (Join-Path $RunDir 'summary.md') -Value ($summary -join "`n") -Encoding utf8
    Write-Step "Tests: $word"
    foreach ($line in $summary | Select-Object -Skip 2) { if ($line) { Write-Note $line } }
    return $code
}

function Invoke-Tests {
    $spec = $Presets[$Preset]
    $runFilter = if ($Filter) { $Filter.Trim() } else { $spec.Filter }
    if ($runFilter -match "[,;'`"]") { throw "The filter may not hold commas, semicolons or quotes: $runFilter" }
    $timeout = if ($TimeoutMinutes -gt 0) { $TimeoutMinutes } else { $spec.Timeout }
    $runDir = Join-Path $projectDir ('Saved\DreamAnimTests\{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Preset)
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $runLog = Join-Path $runDir 'run.log'
    # No value may end in a backslash: the C runtime reads \" as a quote.
    $parts = @(
        "`"$uproject`"",
        "-ExecCmds=`"Automation RunTests $runFilter`"",
        '-TestExit="Automation Test Queue Empty"',
        "-ReportExportPath=`"$($runDir.TrimEnd('\'))`"",
        "-abslog=`"$runLog`"",
        '-unattended', '-nopause', '-nosplash', '-NoSound', '-nullrhi', '-NoLiveCoding'
    )
    $argLine = $parts -join ' '
    Set-Content -LiteralPath (Join-Path $runDir 'command.txt') -Value "`"$editorCmd`" $argLine" -Encoding utf8
    Write-Step "Running $runFilter"
    Write-Note "log: $runLog"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $proc = Start-Process -FilePath $editorCmd -ArgumentList $argLine -WorkingDirectory $projectDir -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $runDir 'stdout.txt') -RedirectStandardError (Join-Path $runDir 'stderr.txt')
    $null = $proc.Handle
    if (-not $proc.WaitForExit($timeout * 60 * 1000)) {
        $timedOut = $true
        Write-Problem "Still running after $timeout minutes: killing the editor."
        try { $proc.Kill($true) } catch { }
        $null = $proc.WaitForExit(60000)
    }
    $watch.Stop()
    $exitCode = if ($timedOut) { $null } else { $proc.ExitCode }
    return (Get-Digest -RunDir $runDir -Floor $spec.MinTests -TimedOut $timedOut -ExitCode $exitCode -Seconds $watch.Elapsed.TotalSeconds)
}

switch ($Command) {
    'build' { if (Invoke-Build) { exit 0 } else { exit 2 } }
    'test' {
        if (-not $NoBuild) { if (-not (Invoke-Build)) { exit 2 } }
        exit (Invoke-Tests)
    }
}
