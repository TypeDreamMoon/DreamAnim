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
    Write-Host 'Tool regression checks passed.'
}
finally {
    $env:ProgramData = $oldProgramData
    $resolved = (Resolve-Path -LiteralPath $scratch).Path
    if ((Split-Path $resolved -Leaf) -notlike 'dab-tools-*') { throw 'Unsafe test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
