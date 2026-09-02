[CmdletBinding()]
param(
    [switch]$Check,
    [string]$LUpdate
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$syncScript = Join-Path $PSScriptRoot 'sync.py'

if (-not $LUpdate) {
    $bundled = Join-Path $projectRoot 'tools\Qt\6.8.3\mingw_64\bin\lupdate.exe'
    if (Test-Path -LiteralPath $bundled) {
        $LUpdate = $bundled
    } else {
        $command = Get-Command lupdate -ErrorAction SilentlyContinue
        if ($command) {
            $LUpdate = $command.Source
        }
    }
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
