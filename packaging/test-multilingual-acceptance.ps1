[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out',
    [string]$PayloadRoot = 'dist\.program-payload',
    [string]$PortableZip = '',
    [string]$UpdateZip = '',
    [string]$SetupPath = ''
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
$identity = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'distribution-identity.psd1')
$python = (Get-Command python -ErrorAction Stop).Source
$ctest = Join-Path $root 'tools\cmake\bin\ctest.exe'
if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) {
    $ctest = (Get-Command ctest.exe -ErrorAction Stop).Source
}

function Resolve-ProjectPath([string]$Path) {
    $resolved = if ([System.IO.Path]::IsPathRooted($Path)) {
        [System.IO.Path]::GetFullPath($Path)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $root $Path))
    }
    if (-not $resolved.StartsWith(($root.TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Multilingual acceptance path escaped the repository: $resolved"
    }
    return $resolved
}

function Invoke-Checked([string]$Label, [scriptblock]$Action) {
    Write-Host "[multilingual-acceptance] $Label"
    & $Action
    if ($LASTEXITCODE -ne 0) { throw "$Label failed with exit code $LASTEXITCODE." }
}

$build = Resolve-ProjectPath $BuildDirectory
$payload = Resolve-ProjectPath $PayloadRoot
$releaseRoot = Join-Path $root 'dist\releases'
if ([string]::IsNullOrWhiteSpace($PortableZip)) {
    $PortableZip = Join-Path $releaseRoot ($identity.ArtifactPatterns.Portable -f $version)
}
if ([string]::IsNullOrWhiteSpace($UpdateZip)) {
    $UpdateZip = Join-Path $releaseRoot ($identity.ArtifactPatterns.Update -f $version)
}
if ([string]::IsNullOrWhiteSpace($SetupPath)) {
    $SetupPath = Join-Path $releaseRoot ($identity.ArtifactPatterns.Setup -f $version)
}
$portable = Resolve-ProjectPath $PortableZip
$update = Resolve-ProjectPath $UpdateZip
$setup = Resolve-ProjectPath $SetupPath
foreach ($required in @($build, $payload, $portable, $update, $setup)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing acceptance input: $required" }
}

$workRoot = Join-Path $root 'out\multilingual-acceptance'
$reportPath = Join-Path $workRoot 'report.json'
if (Test-Path -LiteralPath $workRoot) {
    Remove-Item -LiteralPath $workRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $workRoot -Force | Out-Null

$before = [string]::Join("`n", @(& git -C $root status --porcelain=v1 --untracked-files=all))
if ($LASTEXITCODE -ne 0) { throw 'Could not snapshot the checkout before acceptance.' }
$env:GAMEHQ_OFFLINE_ACCEPTANCE = '1'
$env:PATH = (Join-Path $root 'tools\Qt\6.8.3\mingw_64\bin') + ';' + $env:PATH

Invoke-Checked 'fast offline sixteen-locale gate' {
    & (Join-Path $root 'tools\i18n\ci.ps1')
}

$focused = '^(tst_localizationcatalog|tst_languagemanager|tst_languagepreference|tst_liveretranslation|tst_releasenotes|tst_package_localization|tst_multilingualacceptancereport)$'
Invoke-Checked 'focused runtime, persistence, fallback, footer, and corruption tests' {
    & $ctest --test-dir $build -R $focused --output-on-failure
}

Invoke-Checked 'actual payload, portable, and update archive matrix' {
    & (Join-Path $PSScriptRoot 'test-localized-package.ps1') `
        -PayloadRoot $payload -PortableZip $portable -UpdateZip $update
}

Invoke-Checked 'isolated installer handoff and representative lifecycle matrix' {
    & (Join-Path $PSScriptRoot 'test-installer-language-acceptance.ps1') -SetupPath $setup
}

$packageReport = Join-Path $root 'out\package-localization\report.json'
$bootstrapReport = Join-Path $root 'out\installer-bootstrap\report.json'
$regressionReport = Join-Path $root 'out\installer-regression\report.json'
Invoke-Checked 'deterministic per-locale evidence merge' {
    & $python (Join-Path $root 'tools\i18n\multilingual_acceptance_report.py') `
        --root $root --package-report $packageReport --bootstrap-report $bootstrapReport `
        --regression-report $regressionReport --output $reportPath
}

$report = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($report.production_locale_count -ne 16 -or $report.passed_locale_count -ne 16 `
    -or $report.failed_locale_count -ne 0 -or $report.linguistic_acceptance `
    -or $report.external_browser_opened -or $report.update_authorization_input) {
    throw 'Final multilingual acceptance summary violates its sixteen-locale or trust boundary.'
}

$after = [string]::Join("`n", @(& git -C $root status --porcelain=v1 --untracked-files=all))
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the checkout after acceptance.' }
if ($after -cne $before) {
    throw "Multilingual acceptance modified the checkout.`nBefore:`n$before`nAfter:`n$after"
}

Write-Host '[multilingual-acceptance] ALL 16/16 PRODUCTION LOCALES PASSED'
Write-Host '[multilingual-acceptance] technical/runtime acceptance only; linguistic review remains pending'
Write-Host "[multilingual-acceptance] report: $reportPath"
