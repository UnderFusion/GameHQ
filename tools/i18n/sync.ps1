[CmdletBinding()]
param(
    [switch]$Check,
    [string]$LUpdate
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$syncScript = Join-Path $PSScriptRoot 'sync.py'

if (-not $LUpdate) {
    . (Join-Path $projectRoot 'packaging\resolve-qt.ps1')
    $kit = Resolve-GameHQQtKit -Root $projectRoot -QtPrefix $env:GAMEHQ_DEV_QT_PREFIX
    $LUpdate = Join-Path $kit.Bin 'lupdate.exe'
}

if (-not $LUpdate -or -not (Test-Path -LiteralPath $LUpdate)) {
    throw 'Qt lupdate was not found. Pass -LUpdate or install the project Qt toolchain.'
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    throw 'Python 3 is required to run the localization synchronization command.'
}

$arguments = @($syncScript, '--root', $projectRoot, '--lupdate', $LUpdate)
if ($Check) {
    $arguments += '--check'
}

& $python.Source @arguments
exit $LASTEXITCODE
