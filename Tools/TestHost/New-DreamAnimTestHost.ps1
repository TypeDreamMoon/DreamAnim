# Copyright DreamMoon. All Rights Reserved.
#
# Creates (or refreshes) the DreamAnim test host: a minimal code project that builds DreamAnim on its
# own and runs its automation tests away from any working project.
#
#   <HostDir>/                      copy of Tools/TestHost/Template
#   <HostDir>/Plugins/DreamAnim     git worktree of this repository on -Branch (daily development)
#   <HostDir>/Plugins/ALS-Refactored  ALS-Refactored pinned at -AlsCommit: the corpus of real
#                                   Animation Blueprints the tests decompile and regenerate
#
# Running it again re-copies the template and leaves the worktree and the ALS clone alone.

[CmdletBinding()]
param(
    [string]$HostDir = 'I:\UnrealProject_Moon\DEV_58\DreamAnimTestHost',
    [string]$Branch = 'dev',
    [string]$EngineAssociation,
    [string]$AlsSource = 'https://github.com/Sixze/ALS-Refactored.git',
    [string]$AlsCommit = 'b754d6f0f2bb03741d301f8fb88077ebfe561e17'
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$template = Join-Path $PSScriptRoot 'Template'

function Invoke-Git([string[]]$GitArgs) {
    & git @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed (exit $LASTEXITCODE)" }
}

Write-Host "==> Template -> $HostDir"
New-Item -ItemType Directory -Force -Path $HostDir | Out-Null
$projectPath = Join-Path $HostDir 'DreamAnimTestHost.uproject'
if (-not $PSBoundParameters.ContainsKey('EngineAssociation')) {
    $EngineAssociation = if (Test-Path -LiteralPath $projectPath) {
        (Get-Content -LiteralPath $projectPath -Raw -Encoding UTF8 | ConvertFrom-Json).EngineAssociation
    } else { '5.8' }
}
Copy-Item -Path (Join-Path $template '*') -Destination $HostDir -Recurse -Force
# Only the association line changes, so the file keeps the template's layout.
$projectText = Get-Content -LiteralPath $projectPath -Raw -Encoding UTF8
$projectText = [regex]::Replace($projectText, '("EngineAssociation"\s*:\s*)"[^"]*"', { param($m) $m.Groups[1].Value + '"' + $EngineAssociation + '"' })
Set-Content -LiteralPath $projectPath -Value $projectText -Encoding UTF8 -NoNewline

$pluginDir = Join-Path $HostDir 'Plugins\DreamAnim'
if (-not (Test-Path $pluginDir)) {
    Write-Host "==> Worktree $Branch -> $pluginDir"
    & git -C $repo rev-parse --verify --quiet "refs/heads/$Branch" | Out-Null
    if ($LASTEXITCODE -ne 0) { Invoke-Git @('-C', $repo, 'branch', $Branch) }
    Invoke-Git @('-C', $repo, 'worktree', 'add', $pluginDir, $Branch)
} else {
    Write-Host "    worktree already there: $pluginDir"
}

$alsDir = Join-Path $HostDir 'Plugins\ALS-Refactored'
if (-not (Test-Path $alsDir)) {
    Write-Host "==> ALS-Refactored $($AlsCommit.Substring(0, 8)) -> $alsDir"
    Invoke-Git @('clone', '--quiet', $AlsSource, $alsDir)
    Invoke-Git @('-C', $alsDir, 'checkout', '--quiet', '--detach', $AlsCommit)
} else {
    $head = & git -C $alsDir rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read the ALS-Refactored revision.' }
    $head = ([string]$head).Trim()
    if ($head -ne $AlsCommit) { Write-Warning "ALS-Refactored is at $head, not the pinned $AlsCommit" }
    else { Write-Host "    ALS-Refactored already at the pinned commit" }
}

$info = [ordered]@{
    schema            = 1
    repoPath          = $repo
    worktreePath      = $pluginDir
    branch            = $Branch
    alsCommit         = $AlsCommit
    engineAssociation = $EngineAssociation
}
$info | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $HostDir '.dreamanim-testhost.json') -Encoding utf8

Write-Host "==> Done. Build and test with: pwsh -File `"$pluginDir\Tools\dab.ps1`" test"
