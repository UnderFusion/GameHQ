[CmdletBinding()]
param(
    [string]$PythonExecutable = 'python'
)

$ErrorActionPreference = 'Stop'
$env:PYTHONIOENCODING = 'utf-8'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$sourceRoot = Join-Path $root 'assets\release-notes'
$generatedRoot = Join-Path $sourceRoot 'generated'

function Invoke-PythonCheck {
    param([string[]]$Arguments, [string]$Label)
    & $PythonExecutable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE."
    }
}

Invoke-PythonCheck -Label 'Release-note source tests' -Arguments @(
    (Join-Path $root 'tools\i18n\test_release_notes_generation.py')
)
Invoke-PythonCheck -Label 'Release-note publication tests' -Arguments @(
    (Join-Path $root 'tools\i18n\test_release_publication.py')
)
Invoke-PythonCheck -Label 'Release-note bundle freshness' -Arguments @(
    (Join-Path $root 'tools\i18n\generate_release_notes.py'), '--check', '--launch-status'
)
Invoke-PythonCheck -Label 'Release-note publication freshness' -Arguments @(
    (Join-Path $root 'tools\i18n\generate_release_publication.py'), '--check'
)

$localeManifest = Get-Content (Join-Path $root 'i18n\locales.json') -Raw | ConvertFrom-Json
$expectedLocales = @($localeManifest.locales | Where-Object {
    $_.state -eq 'enabled' -and $_.tier -eq 1
} | ForEach-Object { [string]$_.tag })
$indexPath = Join-Path $generatedRoot 'release-notes.index.json'
$index = Get-Content $indexPath -Raw | ConvertFrom-Json
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()

if ($expectedLocales.Count -ne 16 -or @($expectedLocales | Select-Object -Unique).Count -ne 16) {
    throw 'The locale registry must expose exactly sixteen unique production locales.'
}
if ($expectedLocales -contains 'en-XA' -or $expectedLocales -contains 'ar-XB') {
    throw 'Pseudo-locales cannot enter release-note acceptance.'
}
if ($index.schema_version -ne 2 -or $index.source_locale -ne 'en-US' `
    -or $index.history_limit -ne 12 -or $index.current_version -ne $version) {
    throw "Release-note index metadata does not match schema 2 and VERSION $version."
}
$actualLocales = @($index.bundles | ForEach-Object { [string]$_.locale })
if (($actualLocales -join "`n") -cne ($expectedLocales -join "`n")) {
    throw 'Release-note bundle order or locale coverage differs from the production registry.'
}

$strictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
$replacement = [string][char]0xfffd
$assetPaths = @(
    Get-ChildItem (Join-Path $sourceRoot 'versions') -File -Recurse -Filter '*.json'
    Get-ChildItem $generatedRoot -File -Filter '*.json'
    Get-ChildItem (Join-Path $sourceRoot 'publication') -File -Recurse |
        Where-Object { $_.Extension -in @('.json', '.md') }
)
foreach ($asset in $assetPaths) {
    try {
        $text = $strictUtf8.GetString([System.IO.File]::ReadAllBytes($asset.FullName))
    } catch {
        throw "Malformed UTF-8 release-note asset: $($asset.FullName)"
    }
    if ($text.Contains($replacement)) {
        throw "Unicode replacement character found in release-note asset: $($asset.FullName)"
    }
}

foreach ($record in $index.bundles) {
    $expectedName = "release-notes.$($record.locale).json"
    if ($record.filename -cne $expectedName) {
        throw "$($record.locale): expected bundle filename $expectedName, got $($record.filename)."
    }
    $path = Join-Path $generatedRoot $record.filename
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "$($record.locale): generated bundle is missing: $path"
    }
    $item = Get-Item -LiteralPath $path
    if ($item.Length -ne [long]$record.size) {
        throw "$($record.locale): bundle byte size does not match the index."
    }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -cne [string]$record.sha256) {
        throw "$($record.locale): bundle SHA-256 does not match the index."
    }
}

Write-Host '[release-notes] acceptance passed: 16 locales, sources, bundles, publication, UTF-8, and integrity'
