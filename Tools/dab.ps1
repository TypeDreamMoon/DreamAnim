# Copyright DreamMoon. All Rights Reserved.
#
# DreamAnim command line.
#
#   dab.ps1 make                  build the test host's editor target (the C++)
#   dab.ps1 test [-Preset P] [-Filter F] [-NoBuild]
#                                 make, then run the automation tests headless in the test host. Presets:
#                                 Quick (the language, the checker's corpus, the driver, write-back, the
#                                 editor; the default), RoundTrip (every ALS Blueprint decompiled, checked,
#                                 generated and compared, plain and with an animation set), Behavior (the
#                                 ALS character and generated Blueprints run and their poses compared), Rig
#                                 (the .dcr front end, the engine's Control Rigs read and written as text,
#                                 and write-back), RigRoundTrip (those rigs rebuilt from their text and
#                                 compared as text and as models), RigBehavior (rebuilt rigs run side by
#                                 side with the originals, a modular rig with rebuilt modules included), All.
#                                 -Filter replaces the preset's tests.
#
#   dab.ps1 build [-File F] [-Root R] [-NoSave] [-Json J]
#                                 compile the .dab/.dai sources into Animation Blueprint assets and save them
#   dab.ps1 check [-File F] [-Root R] [-Json J]
#                                 parse, includes, templates and semantic only; nothing is generated
#   dab.ps1 fmt [-File F] [-Root R] [-Write | -Verify] [-Markers] [-Json J]
#                                 print the canonical text; -Write rewrites the files that differ (keeping
#                                 their line ends); -Verify writes nothing and fails on any file not canonical
#   dab.ps1 decompile (-Asset A[,B] | -Under /Game/Folder) [-AnimSet S] [-Out O] [-Stdout [-Markers]] [-Force] [-Json J]
#                                 Animation Blueprints to text, below the project's DAnim/ unless -Out;
#                                 a file that exists and differs is only replaced with -Force; -AnimSet moves
#                                 their animations into slots of that animation set (created when missing)
#   dab.ps1 vocab (-Out O | -Stdout) [-RigUnits]
#                                 what the languages know, as JSON: the grammar version, the words of each kind
#                                 of document, every anim node type with its pins; -RigUnits adds the Control
#                                 Rig units and dispatches (Docs/Tools.md)
#   dab.ps1 dal [-File F] [-Root R] [-Dal D] [-DalModule M]
#                                 compile the .dal logic sources into C++ with the dal compiler (see below)
#
# build, check, fmt, decompile and vocab run `UnrealEditor-Cmd <project> -run=DreamAnim` on -Project, else the
# project above -File, else the test host, and keep the lines that matter: diagnostics
# (`<file>(<line>,<col>): error DABnnnn: ...`), the summary, errors and crashes (-Raw shows everything).
# -File is a source (absolute, relative to the current directory, or relative to the project directory);
# a .dat stands for the documents that include it. -Root replaces the project's and the enabled plugins'
# DAnim/ folders. -Out and -Json are relative to the project directory. -Json writes every diagnostic as
# JSON for the VS Code extension. fmt and decompile -Stdout print the text on the output stream and their
# status lines on the host; several files' texts come between `===== begin <name> =====` and
# `===== end <name> =====` lines, and -Markers puts those lines around a single text too (a host line
# also reaches a redirected stdout, so a tool reading it needs them). Redirected output is UTF-8.
# The engine log of each run is kept in <project>/Saved/DreamAnim/.
# The engine is the project's EngineAssociation, looked up among registered source builds and Launcher
# installations; -EngineRoot <dir> names one instead.
# A build writes packages: close any editor that has the project open.
#
# .dal logic sources are compiled by the dal compiler, which is not part of the plugin: -Dal, else the
# DREAMANIM_DAL environment variable, names the folder that holds its `dal` package; without either, .dal
# files are left alone. Python is DREAMANIM_PYTHON, else `python`. The sources are the .dal files under the
# project's DAnim/ and the DAnim/ of its plugins (or -Root, or -File). Each one's C++ goes to
# Source/<Module>/Public/Dal/ of the project or plugin it is in, with <MODULE>_API on its class: the
# module is -DalModule, else the descriptor's first Runtime module. `build` compiles them first and stops
# on an error; when C++ changed it builds the project's editor target before the .dab sources (-NoBuild
# skips that). `check` checks them as well. `dal` only compiles. Their diagnostics print as the
# commandlet's do: `<file>(<line>,<col>): error DALnnnn: ...`.
#
# Exit codes
#   make: 0 built, 2 failed.
#   test: 0 green, 1 red (a test failed or an ensure fired), 2 not judged (the build failed, the editor
#         crashed or timed out, or fewer tests ran than the preset expects). Results land in
#         <host>/Saved/DreamAnimTests/<time>-<preset>/: the engine's index.json, the run log and summary.md.
#   build, check, fmt, decompile, vocab: 0 no errors, 1 errors (fmt -Verify: a file not canonical), 2 the
#         commandlet did not finish (it crashed, timed out, or never printed its summary).
#   dal: 0 compiled, 1 errors in a source, 2 no compiler, a source in no module, or the compiler failed.
#         build and check take 2 from the dal step the same way, and build takes 2 when the C++ build fails.

[CmdletBinding()]
param(
    [Parameter(Position = 0)][ValidateSet('make', 'test', 'build', 'check', 'fmt', 'decompile', 'vocab', 'dal')][string]$Command = 'test',
    [ValidateSet('Quick', 'RoundTrip', 'Behavior', 'Rig', 'RigRoundTrip', 'RigBehavior', 'All')][string]$Preset = 'Quick',
    [string]$Filter,
    [switch]$NoBuild,
    [string]$Project,
    [string]$EngineRoot,
    [int]$TimeoutMinutes = 0,
    [string]$File,
    [string[]]$Root,
    [switch]$Write,
    [switch]$Verify,
    [string[]]$Asset,
    [string]$Under,
    [string]$AnimSet,
    [string]$Out,
    [switch]$Stdout,
    [string]$Json,
    [switch]$NoSave,
    [switch]$Force,
    [switch]$Raw,
    [switch]$Markers,
    [switch]$RigUnits,
    [string]$Dal,
    [string]$DalModule
)

$ErrorActionPreference = 'Stop'

# A redirected stdout is otherwise written in the console's code page, which mangles Chinese names. An
# interactive console keeps its own setting.
if ([Console]::IsOutputRedirected) { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) }

# MinTests is the least a preset runs: fewer means the filter or the module is broken, not a green run.
$Presets = @{
    Quick     = @{ Filter = 'DreamAnim.Lang+DreamAnim.Driver+DreamAnim.Runtime+DreamAnim.WriteBack+DreamAnim.Editor'; MinTests = 100; Timeout = 30 }
    RoundTrip = @{ Filter = 'DreamAnim.Corpus.Als.Compiles+DreamAnim.Corpus.Als.Decompile+DreamAnim.Corpus.Als.Semantic+DreamAnim.Corpus.Als.Generate+DreamAnim.Corpus.Als.AnimSet.'; MinTests = 100; Timeout = 60 }
    Behavior  = @{ Filter = 'DreamAnim.Corpus.Als.PoseHarness+DreamAnim.Corpus.Als.PoseDiff+DreamAnim.Corpus.Als.AnimSetPoseDiff+DreamAnim.Corpus.Als.AnimSetSwap+DreamAnim.Generate'; MinTests = 5; Timeout = 60 }
    Rig       = @{ Filter = 'DreamAnim.Lang.Dcr+DreamAnim.Rig.Extract+DreamAnim.Rig.Decompile+DreamAnim.Rig.Semantic+DreamAnim.Rig.WriteBack+DreamAnim.Rig.Vocabulary'; MinTests = 50; Timeout = 30 }
    RigRoundTrip = @{ Filter = 'DreamAnim.Rig.Generate+DreamAnim.Rig.RoundTrip'; MinTests = 45; Timeout = 90 }
    RigBehavior  = @{ Filter = 'DreamAnim.Rig.Behavior+DreamAnim.Rig.Generate.RoundTrip.Standalone'; MinTests = 2; Timeout = 60 }
    All       = @{ Filter = 'DreamAnim.'; MinTests = 200; Timeout = 90 }
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

function Find-ProjectAbove([string]$Path) {
    $dir = Split-Path -Parent $Path
    while ($dir) {
        $found = Get-ChildItem -LiteralPath $dir -Filter '*.uproject' -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { return $found.FullName }
        $parent = Split-Path -Parent $dir
        if ($parent -eq $dir) { break }
        $dir = $parent
    }
    return $null
}

function Resolve-EngineRoot([string]$UProject) {
    $assoc = (Get-Content -LiteralPath $UProject -Raw -Encoding UTF8 | ConvertFrom-Json).EngineAssociation
    $root = $EngineRoot
    if (-not $root) {
        $builds = Get-ItemProperty 'HKCU:\Software\Epic Games\Unreal Engine\Builds' -ErrorAction SilentlyContinue
        $root = if ($builds) { $builds.$assoc } else { $null }
        if (-not $root) {
            # A launcher install of another project's engine.
            $launcher = Get-ItemProperty "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$assoc" -ErrorAction SilentlyContinue
            if ($launcher) { $root = $launcher.InstalledDirectory }
        }
        if (-not $root) {
            $manifest = Join-Path $env:ProgramData 'Epic\UnrealEngineLauncher\LauncherInstalled.dat'
            if (Test-Path -LiteralPath $manifest) {
                $installs = (Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json).InstallationList
                $install = $installs | Where-Object { $_.AppName -eq "UE_$assoc" } | Select-Object -First 1
                if ($install) { $root = $install.InstallLocation }
            }
        }
    }
    if (-not $root) { throw "Engine association $assoc is not registered." }
    # Only what building needs: a source build gets its editor from the first `make`.
    foreach ($required in @('Engine\Build\Build.version', 'Engine\Build\BatchFiles\Build.bat')) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $required) -PathType Leaf)) {
            throw "Engine $root is missing $required. Pass -EngineRoot to select a complete installation."
        }
    }
    return $root.Replace('/', '\')
}

# -File relative to the current directory, when it names a file there; else as given (the commandlet
# reads it relative to the project directory).
$sourceFile = $File
if ($File -and (Test-Path -LiteralPath $File -PathType Leaf)) { $sourceFile = (Resolve-Path -LiteralPath $File).Path }

$uproject = if ($Command -in 'make', 'test') { Find-HostProject }
elseif ($Project) { (Resolve-Path $Project).Path }
elseif ($sourceFile -and [IO.Path]::IsPathRooted($sourceFile) -and (Find-ProjectAbove $sourceFile)) { Find-ProjectAbove $sourceFile }
else { Find-HostProject }
$projectDir = Split-Path $uproject
$hostName = [IO.Path]::GetFileNameWithoutExtension($uproject)
# `dal` runs no engine, so it needs none.
$engineRoot = if ($Command -ne 'dal') { Resolve-EngineRoot $uproject }
$editorCmd = if ($engineRoot) { Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe' }

function Invoke-Build {
    Write-Step "Building ${hostName}Editor"
    $log = Join-Path $projectDir 'Saved\DreamAnimTests\build.log'
    New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
    $buildBat = Join-Path $engineRoot 'Engine\Build\BatchFiles\Build.bat'
    & $buildBat "${hostName}Editor" Win64 Development "-Project=$uproject" -WaitMutex -NoHotReloadFromIDE *> $log
    $code = $LASTEXITCODE
    $errors = @(Select-String -LiteralPath $log -Pattern 'error C\d+|error LNK\d+|error :|\): error' | Select-Object -First 15 | ForEach-Object { $_.Line.Trim() })
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
    $incomplete = @($tests | Where-Object { $_.State -notin 'Success', 'Fail', 'Skipped' })
    # The engine exits with -1 when a test failed or logged an error: a red run, not a broken one. Any other
    # exit, or -1 with nothing failed, means the editor stopped for a reason the report does not show.
    $redExit = $ExitCode -eq -1 -and ($failed.Count -gt 0 -or $ensures.Count -gt 0)
    if (-not $TimedOut -and ($null -eq $ExitCode -or ($ExitCode -ne 0 -and -not $redExit))) { $infra += "The editor did not exit successfully (exit $ExitCode)." }
    if ($incomplete.Count -gt 0) { $infra += "$($incomplete.Count) test(s) did not finish: $($incomplete.Path -join ', ')." }
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
        foreach ($i in ($t.Infos | Select-Object -First 40)) { $summary += "    - $i" }
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
    $floor = if ($Filter) { 1 } else { $spec.MinTests }
    return (Get-Digest -RunDir $runDir -Floor $floor -TimedOut $timedOut -ExitCode $exitCode -Seconds $watch.Elapsed.TotalSeconds)
}

# ---------------------------------------------------------------- build / check / fmt / decompile

function Get-EditorsOnProject([string]$UProject) {
    # Every UnrealEditor*.exe whose command line names this .uproject.
    $needle = $UProject.Replace('/', '\').ToLowerInvariant()
    foreach ($p in @(Get-CimInstance -ClassName Win32_Process -Filter "Name LIKE 'UnrealEditor%'" -ErrorAction SilentlyContinue)) {
        $line = [string]$p.CommandLine
        if ($line -and $line.Replace('/', '\').ToLowerInvariant().Contains($needle)) { $p }
    }
}

function Format-Switch([string]$Name, [string]$Value) {
    # One argument of the command line, quoted. A value may not end in a backslash (the C runtime reads \"
    # as a quote) nor hold a quote.
    if ($Value.Contains('"')) { throw "-$Name may not hold a quote: $Value" }
    return "-$Name=`"$($Value.TrimEnd('\'))`""
}

$script:CommandletExit = 2

function Invoke-Commandlet {
    $arguments = @()
    switch ($Command) {
        'check' { $arguments += '-Check' }
        'fmt' { $arguments += '-Format' }
        'decompile' { $arguments += '-Decompile' }
        'vocab' { $arguments += '-Vocabulary' }
    }
    if ($sourceFile) { $arguments += Format-Switch 'File' $sourceFile }
    if ($Root) { $arguments += Format-Switch 'Root' (($Root | ForEach-Object { $_.TrimEnd('\') }) -join ',') }
    if ($Write) { $arguments += '-Write' }
    if ($Verify) { $arguments += '-Verify' }
    foreach ($a in @($Asset)) { if ($a) { $arguments += Format-Switch 'Asset' $a } }
    if ($Under) { $arguments += Format-Switch 'Under' $Under }
    if ($AnimSet) { $arguments += Format-Switch 'AnimSet' $AnimSet }
    if ($Out) { $arguments += Format-Switch 'Out' $Out }
    if ($Stdout) { $arguments += '-Stdout' }
    if ($Json) { $arguments += Format-Switch 'Json' $Json }
    if ($NoSave) { $arguments += '-NoSave' }
    if ($Force) { $arguments += '-Force' }
    if ($RigUnits) { $arguments += '-RigUnits' }

    $writesAssets = $Command -eq 'build'
    if ($writesAssets -and @(Get-EditorsOnProject $uproject).Count -gt 0) {
        Write-Problem "An editor has $uproject open; a build that saves packages will fight it over them."
    }

    $runDir = Join-Path $projectDir 'Saved\DreamAnim'
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    $stamp = '{0}-{1}' -f $Command, (Get-Date -Format 'yyyyMMdd-HHmmss')
    $log = Join-Path $runDir "$stamp.log"
    $stdoutFile = Join-Path $runDir "$stamp.stdout.txt"
    $stderrFile = Join-Path $runDir "$stamp.stderr.txt"
    # The log goes to a file, not to stdout: the engine's own -stdout switch is also the commandlet's
    # "print the decompiled text" switch, and stdout carries only the text this way.
    $parts = @("`"$uproject`"", '-run=DreamAnim') + $arguments + @(
        "-abslog=`"$log`"", '-unattended', '-nopause', '-nosplash', '-NoSound', '-nullrhi', '-NoLiveCoding')
    $argLine = $parts -join ' '
    Write-Verbose "`"$editorCmd`" $argLine"

    $proc = Start-Process -FilePath $editorCmd -ArgumentList $argLine -WorkingDirectory $projectDir -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile
    $null = $proc.Handle
    $timedOut = $false
    $limit = if ($TimeoutMinutes -gt 0) { $TimeoutMinutes * 60 * 1000 } else { -1 }
    if (-not $proc.WaitForExit($limit)) {
        $timedOut = $true
        try { $proc.Kill($true) } catch { }
        $null = $proc.WaitForExit(60000)
    }
    $exitCode = if ($timedOut) { $null } else { $proc.ExitCode }

    $stdoutLines = if (Test-Path -LiteralPath $stdoutFile) { @(Get-Content -LiteralPath $stdoutFile -Encoding utf8) } else { @() }
    $logLines = if (Test-Path -LiteralPath $log) { @(Get-Content -LiteralPath $log -Encoding utf8) } else { @() }

    # fmt and decompile -Stdout print text: that goes on the output stream, everything else on the host.
    $printsText = ($Command -eq 'fmt' -and -not $Write -and -not $Verify) -or ($Command -in 'decompile', 'vocab' -and $Stdout)
    $status = if ($printsText) { { param($Line) Write-Host $Line } } else { { param($Line) Write-Output $Line } }

    if ($Raw) {
        $stdoutLines | ForEach-Object { Write-Output $_ }
        $logLines | ForEach-Object { & $status $_ }
    } else {
        # The text blocks, between the commandlet's markers; a single one without them unless -Markers.
        $blocks = [Collections.Generic.List[object]]::new()
        $current = $null
        foreach ($line in $stdoutLines) {
            if ($line -match '^===== begin (.*) =====$') { $current = [pscustomobject]@{ Name = $Matches[1]; Lines = [Collections.Generic.List[string]]::new() }; continue }
            if ($line -match '^===== end (.*) =====$') { if ($current) { $blocks.Add($current); $current = $null }; continue }
            if ($current) { $current.Lines.Add($line) }
        }
        $marked = $Markers -or $blocks.Count -gt 1
        foreach ($block in $blocks) {
            if ($marked) { Write-Output "===== begin $($block.Name) =====" }
            $block.Lines | ForEach-Object { Write-Output $_ }
            if ($marked) { Write-Output "===== end $($block.Name) =====" }
        }

        # The commandlet's own lines in full; from the rest of the engine only errors and crashes.
        foreach ($line in $logLines) {
            $clean = ($line -replace '^\[[^\]]*\]\[\s*\d+\]', '').TrimEnd()
            if ($clean -match '^LogDreamAnimCommandlet: (?:(?:Display|Warning|Error|Log): )?(.*)$') { & $status $Matches[1]; continue }
            if ($clean -match '=== Critical error|Fatal error|Unhandled Exception|Assertion failed|: Error: |DAB\d{4}') { & $status $clean }
        }
    }

    $crashed = @($logLines + $stdoutLines | Where-Object { $_ -match '=== Critical error: ===|Fatal error!|Unhandled Exception|Assertion failed:' }).Count -gt 0
    $finished = @($logLines | Where-Object { $_ -match 'LogDreamAnimCommandlet: (?:Display: )?DreamAnim( \w+)?: ' }).Count -gt 0
    $script:CommandletExit = 0
    if ($timedOut) { Write-Problem "Still running after $TimeoutMinutes minutes: the editor was killed. Log: $log"; $script:CommandletExit = 2 }
    elseif ($crashed) { Write-Problem "The editor crashed. Log: $log"; $script:CommandletExit = 2 }
    elseif (-not $finished) { Write-Problem "The commandlet did not finish (exit $exitCode). Log: $log"; $script:CommandletExit = 2 }
    elseif ($exitCode -ne 0) { $script:CommandletExit = 1 }
}

# ---------------------------------------------------------------- .dal logic

function Get-DalCompiler([string]$Dir) {
    # The folder that holds the compiler's `dal` package; none when not configured.
    if (-not $Dir) { return $null }
    if (-not (Test-Path -LiteralPath (Join-Path $Dir 'dal\__main__.py') -PathType Leaf)) {
        throw "No dal compiler in ${Dir}: it should hold the dal package (dal\__main__.py)."
    }
    return (Resolve-Path -LiteralPath $Dir).Path.TrimEnd('\')
}

function Get-DalRoots([string]$ProjectDir) {
    # The project's DAnim/ and the DAnim/ of the plugins inside the project.
    $roots = @(Join-Path $ProjectDir 'DAnim')
    $plugins = Join-Path $ProjectDir 'Plugins'
    if (Test-Path -LiteralPath $plugins -PathType Container) {
        foreach ($descriptor in Get-ChildItem -LiteralPath $plugins -Recurse -Depth 3 -Filter '*.uplugin' -File -ErrorAction SilentlyContinue) {
            $roots += Join-Path $descriptor.DirectoryName 'DAnim'
        }
    }
    return @($roots | Where-Object { Test-Path -LiteralPath $_ -PathType Container })
}

function Get-DalSources([string[]]$Roots, [string]$File, [string]$ProjectDir) {
    if ($File) {
        $path = if (Test-Path -LiteralPath $File -PathType Leaf) { $File } else { Join-Path $ProjectDir $File }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "No such source: $File" }
        return @((Resolve-Path -LiteralPath $path).Path)
    }
    return @(foreach ($r in $Roots) {
        Get-ChildItem -LiteralPath $r -Recurse -Filter '*.dal' -File | Where-Object { $_.Extension -eq '.dal' } |
            Sort-Object FullName | ForEach-Object { $_.FullName }
    })
}

function Get-DalTarget([string]$Source, [string]$Module) {
    # Where a source's C++ goes: Source/<Module>/Public/Dal of the project or plugin the source is in. The
    # module is the one given, else the descriptor's first Runtime module.
    $dir = Split-Path -Parent $Source
    $descriptor = $null
    while ($dir) {
        $descriptor = Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in '.uplugin', '.uproject' } | Select-Object -First 1
        if ($descriptor) { break }
        $parent = Split-Path -Parent $dir
        if (-not $parent -or $parent -eq $dir) { break }
        $dir = $parent
    }
    if (-not $descriptor) { throw "$Source is in no project or plugin, so its C++ has no module to go to." }
    $name = $Module
    if (-not $name) {
        $modules = @((Get-Content -LiteralPath $descriptor.FullName -Raw -Encoding UTF8 | ConvertFrom-Json).Modules)
        $name = ($modules | Where-Object { $_.Type -eq 'Runtime' } | Select-Object -First 1).Name
        if (-not $name) { throw "$($descriptor.Name) has no Runtime module for the C++ of ${Source}: pass -DalModule." }
    }
    $moduleDir = Join-Path $descriptor.DirectoryName "Source\$name"
    if (-not (Test-Path -LiteralPath $moduleDir -PathType Container)) { throw "No module $name at $moduleDir (for $Source)." }
    return [pscustomobject]@{ Out = Join-Path $moduleDir 'Public\Dal'; Api = $name.ToUpperInvariant() + '_API' }
}

function Invoke-Dal([string]$Compiler, [string]$Python, [string]$Mode, [string[]]$Sources, [string]$Module) {
    # `python -m dal check|compile` on each source, from the compiler's folder and with it on PYTHONPATH (not
    # -I, which ignores both). Writes its lines to the output stream; the counts come back through
    # $script:DalErrors (sources with errors), $script:DalFailed (runs that did not get that far) and
    # $script:DalWritten (C++ files the compiler changed).
    $ErrorActionPreference = 'Continue'
    $script:DalErrors = 0
    $script:DalFailed = 0
    $script:DalWritten = @()
    $saved = @{ Path = $env:PYTHONPATH; Encoding = $env:PYTHONIOENCODING; Console = [Console]::OutputEncoding }
    $env:PYTHONPATH = if ($saved.Path) { "$Compiler;$($saved.Path)" } else { $Compiler }
    $env:PYTHONIOENCODING = 'utf-8'
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    $errFile = [IO.Path]::GetTempFileName()
    Push-Location -LiteralPath $Compiler
    try {
        foreach ($source in $Sources) {
            $arguments = @('-m', 'dal', $Mode, $source)
            if ($Mode -eq 'compile') {
                try { $target = Get-DalTarget $source $Module }
                catch { Write-Output "${source}: error: $($_.Exception.Message)"; $script:DalFailed++; continue }
                $arguments += @('--cpp', $target.Out, '--module', $target.Api)
            }
            $lines = @(& $Python @arguments 2> $errFile)
            $code = $LASTEXITCODE
            # The compiler names the file as it was given its name; the full path is the one the editors open.
            $leaf = Split-Path -Leaf $source
            foreach ($line in @(Get-Content -LiteralPath $errFile -Encoding utf8)) {
                if ($line -match '^(.*?):(\d+):(\d+): (error|warning) (DAL\d+): (.*)$') {
                    $file = if ((Split-Path -Leaf $Matches[1]) -eq $leaf) { $source } else { $Matches[1] }
                    Write-Output ('{0}({1},{2}): {3} {4}: {5}' -f $file, $Matches[2], $Matches[3], $Matches[4], $Matches[5], $Matches[6])
                }
                elseif ($line -notmatch ': \d+ error\(s\)$') { Write-Output $line }
            }
            foreach ($line in $lines) {
                if ($line -match '^wrote (.*)$') { $script:DalWritten += $Matches[1]; Write-Output $line }
            }
            if ($code -eq 1) { $script:DalErrors++ }
            elseif ($code -ne 0) { Write-Output "${source}: the dal compiler stopped (exit $code)."; $script:DalFailed++ }
        }
    }
    finally {
        Pop-Location
        Remove-Item -LiteralPath $errFile -ErrorAction SilentlyContinue
        $env:PYTHONPATH = $saved.Path
        $env:PYTHONIOENCODING = $saved.Encoding
        [Console]::OutputEncoding = $saved.Console
    }
}

$script:DalExit = 0
$script:DalWritten = @()
$isDalFile = [bool]($sourceFile -and $sourceFile.EndsWith('.dal', [StringComparison]::OrdinalIgnoreCase))

function Invoke-DalStep([string]$Mode) {
    # The .dal sources of this run, compiled or checked. Writes its lines to the output stream; the verdict
    # comes back through $script:DalExit: 0, 1 errors in a source, 2 not run through.
    $script:DalExit = 0
    $compiler = $null
    try { $compiler = Get-DalCompiler $(if ($Dal) { $Dal } else { $env:DREAMANIM_DAL }) }
    catch { Write-Problem $_.Exception.Message; $script:DalExit = 2; return }
    if (-not $compiler) {
        if ($isDalFile -or $Command -eq 'dal') { Write-Problem 'No dal compiler: pass -Dal <folder> or set DREAMANIM_DAL.'; $script:DalExit = 2 }
        return
    }
    $roots = if ($Root) { @($Root) } else { Get-DalRoots $projectDir }
    $sources = @()
    try { $sources = Get-DalSources $roots $(if ($isDalFile) { $sourceFile } else { $null }) $projectDir }
    catch { Write-Problem $_.Exception.Message; $script:DalExit = 2; return }
    if ($sources.Count -eq 0) {
        if ($Command -eq 'dal') { Write-Note "No .dal sources under $($roots -join ', ')." }
        return
    }
    $python = if ($env:DREAMANIM_PYTHON) { $env:DREAMANIM_PYTHON } else { 'python' }
    Invoke-Dal $compiler $python $Mode $sources $DalModule
    Write-Output ('DreamAnim dal: {0} source(s), {1} with errors, {2} C++ file(s) written' -f $sources.Count, $script:DalErrors, $script:DalWritten.Count)
    $script:DalExit = if ($script:DalFailed -gt 0) { 2 } elseif ($script:DalErrors -gt 0) { 1 } else { 0 }
}

switch ($Command) {
    'make' { if (Invoke-Build) { exit 0 } else { exit 2 } }
    'test' {
        if (-not $NoBuild) { if (-not (Invoke-Build)) { exit 2 } }
        exit (Invoke-Tests)
    }
    'dal' {
        Invoke-DalStep 'compile'
        exit $script:DalExit
    }
    default {
        if ($Command -in 'build', 'check') {
            Invoke-DalStep $(if ($Command -eq 'build') { 'compile' } else { 'check' })
            # A build stops on any error in the logic; a check goes on to the other sources.
            if ($script:DalExit -eq 2 -or ($Command -eq 'build' -and $script:DalExit -ne 0)) { exit $script:DalExit }
            # The .dab sources name the generated classes as their parents: those must be compiled first.
            if ($Command -eq 'build' -and $script:DalWritten.Count -gt 0) {
                if ($NoBuild) { Write-Problem "Generated C++ changed; build ${hostName}Editor before the .dab sources that use it." }
                elseif (-not (Invoke-Build)) { exit 2 }
            }
            if ($isDalFile) { exit $script:DalExit }
        }
        # The function writes its lines to the output stream; its verdict comes back through a variable.
        Invoke-Commandlet
        exit ([Math]::Max($script:CommandletExit, $script:DalExit))
    }
}
