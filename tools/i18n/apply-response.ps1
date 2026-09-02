[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Workset,
    [Parameter(Mandatory = $true)][string]$ResponseDir,
    [Parameter(Mandatory = $true)][string]$UpdatedAt,
    [string]$PolicyRoot
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$arguments = @(
    (Join-Path $PSScriptRoot 'apply_response.py'),
    '--root', $projectRoot,
    '--workset', $Workset,
    '--response-dir', $ResponseDir,
    '--updated-at', $UpdatedAt
)
if ($PolicyRoot) {
    $arguments += @('--policy-root', $PolicyRoot)
}
& python @arguments
exit $LASTEXITCODE
