# Copyright DreamMoon. All Rights Reserved.
# Run with pwsh -NoProfile -File Tools/Tests/Test-DabTools.ps1. No engine or network required.
$ErrorActionPreference = 'Stop'
$toolsDir = Split-Path $PSScriptRoot
$scratchRoot = [System.IO.Path]::GetTempPath()
$scratch = Join-Path $scratchRoot ('dab-tools-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$oldProgramData = $env:ProgramData

function Assert-Equal($Expected, $Actual, [string]$Label) {
    if ($Expected -cne $Actual) { throw "$Label`: expected '$Expected', got '$Actual'" }
}

try {
    $tokens = $null
    $errors = $null
    $tree = [System.Management.Automation.Language.Parser]::ParseInput(
        (Get-Content -LiteralPath (Join-Path $toolsDir 'dab.ps1') -Raw -Encoding UTF8), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors -join "`n") }
    foreach ($name in @('Write-Step', 'Write-Note', 'Get-Digest', 'Resolve-EngineRoot')) {
        $function = $tree.Find({ param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
        }, $false)
        if (-not $function) { throw "Missing function $name" }
        Invoke-Expression $function.Extent.Text
    }

    $run = Join-Path $scratch 'report'
    New-Item -ItemType Directory -Path $run | Out-Null
    foreach ($case in @(
        @{ State = 'Success'; Exit = 0; Expected = 0 },
        @{ State = 'Skipped'; Exit = 0; Expected = 0 },
        @{ State = 'Success'; Exit = 3; Expected = 2 },
        @{ State = 'Success'; Exit = -1; Expected = 2 },
        @{ State = 'InProcess'; Exit = 0; Expected = 2 },
        @{ State = 'NotRun'; Exit = 0; Expected = 2 },
        @{ State = 'Fail'; Exit = -1; Expected = 1 },
        @{ State = 'Fail'; Exit = 3; Expected = 2 }
    )) {
        $tests = @(@{ fullTestPath = 'DreamAnim.Passed'; state = 'Success'; duration = 0; entries = @() },
            @{ fullTestPath = 'DreamAnim.Other'; state = $case.State; duration = 0; entries = @() })
        @{ tests = $tests } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'index.json') -Encoding UTF8
        $actual = Get-Digest $run 1 $false $case.Exit 0 6> (Join-Path $run 'output.log')
        Assert-Equal $case.Expected $actual "Digest $($case.State)/$($case.Exit)"
    }

    # Registry discovery must not be needed for a Launcher manifest or an explicit override.
    function Get-ItemProperty { param($Path, $ErrorAction) return $null }
    $fakeEngine = Join-Path $scratch 'EngineInstall'
    foreach ($file in @('Engine/Build/Build.version', 'Engine/Build/BatchFiles/Build.bat', 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe')) {
        $path = Join-Path $fakeEngine $file
        New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
        Set-Content -LiteralPath $path -Value '' -Encoding UTF8
    }
    $env:ProgramData = Join-Path $scratch 'ProgramData'
    $manifest = Join-Path $env:ProgramData 'Epic/UnrealEngineLauncher/LauncherInstalled.dat'
    New-Item -ItemType Directory -Path (Split-Path $manifest) -Force | Out-Null
    @{ InstallationList = @(@{ AppName = 'UE_5.8'; InstallLocation = $fakeEngine }) } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest -Encoding UTF8
    $project = Join-Path $scratch 'Test.uproject'
    @{ EngineAssociation = '5.8' } | ConvertTo-Json | Set-Content -LiteralPath $project -Encoding UTF8
    $EngineRoot = $null
    Assert-Equal $fakeEngine (Resolve-EngineRoot $project) 'Launcher installation'
    @{ EngineAssociation = '{unregistered}' } | ConvertTo-Json | Set-Content -LiteralPath $project -Encoding UTF8
    $EngineRoot = $fakeEngine
    Assert-Equal $fakeEngine (Resolve-EngineRoot $project) 'Explicit installation'

    # Existing plugin directories avoid all clone/worktree operations; mock the one read-only git call.
    function git { $global:LASTEXITCODE = 0; return 'b754d6f0f2bb03741d301f8fb88077ebfe561e17' }
    $testHost = Join-Path $scratch 'Host'
    New-Item -ItemType Directory -Path (Join-Path $testHost 'Plugins/DreamAnim'), (Join-Path $testHost 'Plugins/ALS-Refactored') -Force | Out-Null
    $hostProject = Join-Path $testHost 'DreamAnimTestHost.uproject'
    $createHost = Join-Path $toolsDir 'TestHost/New-DreamAnimTestHost.ps1'
    & $createHost -HostDir $testHost 6> (Join-Path $scratch 'host.log')
    Assert-Equal '5.8' ((Get-Content -LiteralPath $hostProject -Raw -Encoding UTF8 | ConvertFrom-Json).EngineAssociation) 'New host'
    @{ EngineAssociation = '{custom-source}' } | ConvertTo-Json | Set-Content -LiteralPath $hostProject -Encoding UTF8
    & $createHost -HostDir $testHost 6>> (Join-Path $scratch 'host.log')
    Assert-Equal '{custom-source}' ((Get-Content -LiteralPath $hostProject -Raw -Encoding UTF8 | ConvertFrom-Json).EngineAssociation) 'Refreshed host'
    & $createHost -HostDir $testHost -EngineAssociation '5.8' 6>> (Join-Path $scratch 'host.log')
    Assert-Equal '5.8' ((Get-Content -LiteralPath $hostProject -Raw -Encoding UTF8 | ConvertFrom-Json).EngineAssociation) 'Explicit host override'
    Assert-Equal '5.8' ((Get-Content -LiteralPath (Join-Path $testHost '.dreamanim-testhost.json') -Raw -Encoding UTF8 | ConvertFrom-Json).engineAssociation) 'Host metadata'

    # The .dal step against a stand-in compiler: where sources are found, which module their C++ goes to,
    # what counts as written, and how diagnostics come out.
    foreach ($name in @('Get-DalCompiler', 'Get-DalRoots', 'Get-DalSources', 'Get-DalTarget', 'Invoke-Dal')) {
        $function = $tree.Find({ param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
        }, $false)
        if (-not $function) { throw "Missing function $name" }
        Invoke-Expression $function.Extent.Text
    }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) { Write-Host 'No python on the PATH: the .dal checks were skipped.' }
    else {
        $compiler = Join-Path $scratch 'DalCompiler'
        New-Item -ItemType Directory -Path (Join-Path $compiler 'dal') -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $compiler 'dal/__init__.py') -Value '' -Encoding UTF8
        Set-Content -LiteralPath (Join-Path $compiler 'dal/__main__.py') -Encoding UTF8 -Value @'
import os, sys
mode, path = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8").read()
if "usage" in text:
    sys.exit(2)
if "bad" in text:
    print(os.path.basename(path) + ":2:5: error DAL2004: expected an expression", file=sys.stderr)
    print("    state X = ;", file=sys.stderr)
    print(path + ": 1 error(s)", file=sys.stderr)
    sys.exit(1)
if mode == "compile":
    out, api = sys.argv[sys.argv.index("--cpp") + 1], sys.argv[sys.argv.index("--module") + 1]
    os.makedirs(out, exist_ok=True)
    stem = os.path.splitext(os.path.basename(path))[0]
    for name, body in ((stem + ".h", api), ("DalMath.h", "math")):
        target = os.path.join(out, name)
        if os.path.isfile(target) and open(target, encoding="utf-8").read() == body:
            print("up to date " + target)
        else:
            open(target, "w", encoding="utf-8").write(body)
            print("wrote " + target)
'@
        $dalProject = Join-Path $scratch 'DalProject'
        $plugin = Join-Path $dalProject 'Plugins\Sub'
        foreach ($dir in @('DAnim/Logic', 'Source/Game', 'Source/GameEditor')) { New-Item -ItemType Directory -Path (Join-Path $dalProject $dir) -Force | Out-Null }
        foreach ($dir in @('DAnim', 'Source/SubRuntime')) { New-Item -ItemType Directory -Path (Join-Path $plugin $dir) -Force | Out-Null }
        @{ Modules = @(@{ Name = 'GameEditor'; Type = 'Editor' }, @{ Name = 'Game'; Type = 'Runtime' }) } | ConvertTo-Json -Depth 4 |
            Set-Content -LiteralPath (Join-Path $dalProject 'DalProject.uproject') -Encoding UTF8
        @{ Modules = @(@{ Name = 'SubRuntime'; Type = 'Runtime' }) } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $plugin 'Sub.uplugin') -Encoding UTF8
        $hero = Join-Path $dalProject 'DAnim\Logic\hero.dal'
        $sub = Join-Path $plugin 'DAnim\sub.dal'
        Set-Content -LiteralPath $hero -Value 'logic Hero;' -Encoding UTF8
        Set-Content -LiteralPath $sub -Value 'logic Sub;' -Encoding UTF8
        Set-Content -LiteralPath (Join-Path $dalProject 'DAnim/Logic/notes.dalx') -Value 'not a source' -Encoding UTF8

        Assert-Equal $null (Get-DalCompiler '') 'No compiler configured'
        $threw = $false; try { Get-DalCompiler $dalProject | Out-Null } catch { $threw = $true }
        Assert-Equal $true $threw 'A folder without the dal package'
        Assert-Equal $compiler (Get-DalCompiler $compiler) 'The compiler folder'
        $roots = @(Get-DalRoots $dalProject)
        Assert-Equal 2 $roots.Count 'Roots: the project and its plugin'
        $sources = @(Get-DalSources $roots $null $dalProject)
        Assert-Equal "$hero|$sub" ($sources -join '|') 'Sources: .dal files only'
        Assert-Equal $hero ((Get-DalSources $roots 'DAnim/Logic/hero.dal' $dalProject) -join '|') 'A source relative to the project'
        $target = Get-DalTarget $hero ''
        Assert-Equal (Join-Path $dalProject 'Source\Game\Public\Dal') $target.Out 'The project''s first Runtime module'
        Assert-Equal 'GAME_API' $target.Api 'Its export macro'
        Assert-Equal 'SUBRUNTIME_API' (Get-DalTarget $sub '').Api 'A plugin''s source goes to the plugin'
        Assert-Equal 'GAMEEDITOR_API' (Get-DalTarget $hero 'GameEditor').Api 'An explicit module'
        $threw = $false; try { Get-DalTarget $hero 'Missing' | Out-Null } catch { $threw = $true }
        Assert-Equal $true $threw 'A module that is not there'

        $null = Invoke-Dal $compiler $python.Source 'compile' $sources ''
        Assert-Equal '0/0/4' ('{0}/{1}/{2}' -f $script:DalErrors, $script:DalFailed, $script:DalWritten.Count) 'First compile'
        Assert-Equal 'GAME_API' (Get-Content -LiteralPath (Join-Path $target.Out 'hero.h') -Raw).Trim() 'The macro reached the compiler'
        $null = Invoke-Dal $compiler $python.Source 'compile' $sources ''
        Assert-Equal '0/0/0' ('{0}/{1}/{2}' -f $script:DalErrors, $script:DalFailed, $script:DalWritten.Count) 'Nothing changed'
        Set-Content -LiteralPath $hero -Value 'logic Hero; bad' -Encoding UTF8
        $lines = @(Invoke-Dal $compiler $python.Source 'check' @($hero) '')
        Assert-Equal 1 $script:DalErrors 'A source with errors'
        Assert-Equal "$hero(2,5): error DAL2004: expected an expression" $lines[0] 'A diagnostic as the commandlet writes one'
        Assert-Equal '    state X = ;' $lines[1] 'The source line under it'
        Assert-Equal 2 $lines.Count 'The compiler''s own count is left out'
        Set-Content -LiteralPath $hero -Value 'usage' -Encoding UTF8
        $null = Invoke-Dal $compiler $python.Source 'compile' @($hero) ''
        Assert-Equal '0/1' ('{0}/{1}' -f $script:DalErrors, $script:DalFailed) 'A compiler that stopped'
        $null = Invoke-Dal $compiler $python.Source 'compile' @((Join-Path $scratch 'loose.dal')) ''
        Assert-Equal 1 $script:DalFailed 'A source whose project has no Runtime module'
    }
    Write-Host 'Tool regression checks passed.'
}
finally {
    $env:ProgramData = $oldProgramData
    $resolved = (Resolve-Path -LiteralPath $scratch).Path
    if ((Split-Path $resolved -Leaf) -notlike 'dab-tools-*') { throw 'Unsafe test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
