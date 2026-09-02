[CmdletBinding()]
param(
    [string]$BaseRef = 'HEAD',
    [string]$Output,
    [string]$ResponseDir,
    [string]$LUpdate
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Output) {
    $Output = Join-Path $projectRoot 'out\i18n\change-workset.json'
}

& (Join-Path $PSScriptRoot 'sync.ps1') -Check -LUpdate $LUpdate
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& (Join-Path $PSScriptRoot 'verify.ps1')
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$arguments = @(
    (Join-Path $PSScriptRoot 'check_diff.py'),
    '--root', $projectRoot,
    '--base-ref', $BaseRef,
    '--output', $Output
)
if ($ResponseDir) {
    $arguments += @('--response-dir', $ResponseDir)
}
& python @arguments
exit $LASTEXITCODE
