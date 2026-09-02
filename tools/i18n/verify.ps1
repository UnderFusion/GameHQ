[CmdletBinding()]
param(
    [switch]$UpdateState,
    [switch]$Release
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$verifyScript = Join-Path $PSScriptRoot 'verify.py'
$python = Get-Command python -ErrorAction SilentlyContinue

if (-not $python) {
    throw 'Python 3 is required to run localization verification.'
}

$arguments = @($verifyScript, '--root', $projectRoot)
if ($UpdateState) {
    $arguments += '--update-state'
}
if ($Release) {
    $arguments += '--release'
}

& $python.Source @arguments
exit $LASTEXITCODE
