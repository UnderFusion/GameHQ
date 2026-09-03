[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('add', 'update', 'review', 'disable', 'retire', 'restore', 'verify', 'package', 'status')]
    [string]$Command,

    [Parameter(Position = 1)]
    [string]$Tag,

    [string]$EnglishName,
    [string]$NativeName,
    [ValidateSet('ltr', 'rtl')]
    [string]$Direction = 'ltr',
    [string]$Fallback = 'en-US',
    [string[]]$Alias = @(),
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$lifecycleScript = Join-Path $PSScriptRoot 'locale_lifecycle.py'
$python = Get-Command python -ErrorAction SilentlyContinue

if (-not $python) {
    throw 'Python 3 is required to run the locale lifecycle command.'
}

$arguments = @($lifecycleScript, '--root', $projectRoot, $Command)

if ($Command -in @('add', 'update', 'review', 'disable', 'retire', 'restore')) {
    if (-not $Tag) {
        throw "The '$Command' command requires a locale tag, for example: .\locale.ps1 $Command fr-CA"
    }
    $arguments += $Tag
}

if ($Command -eq 'add') {
    if ($EnglishName) { $arguments += @('--english-name', $EnglishName) }
    if ($NativeName) { $arguments += @('--native-name', $NativeName) }
    $arguments += @('--direction', $Direction, '--fallback', $Fallback)
    foreach ($value in $Alias) { $arguments += @('--alias', $value) }
}

if ($Check -and $Command -in @('add', 'update', 'disable', 'retire', 'restore')) {
    $arguments += '--check'
}

$env:PYTHONIOENCODING = 'utf-8'
& $python.Source @arguments
exit $LASTEXITCODE
