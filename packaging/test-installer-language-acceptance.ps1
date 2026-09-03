[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SetupPath
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$toolchain = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'inno-toolchain.psd1')
$compilerRoot = Join-Path $root "tools\InnoSetup\$($toolchain.Version)"
$python = (Get-Command python -ErrorAction Stop).Source
$workRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'out\installer-language-acceptance'))
$reportPath = Join-Path $workRoot 'report.json'
$productionSetup = [System.IO.Path]::GetFullPath($SetupPath)
$staticChecks = [System.Collections.Generic.List[object]]::new()

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-StaticCheck {
    param([string]$Name, [string]$Script, [string[]]$Arguments = @())
    Write-Host "[installer-language-acceptance] STATIC: $Name"
    $output = @(& $python $Script @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode."
    }
    $script:staticChecks.Add([ordered]@{
        name = $Name
        status = 'passed'
        output = ($output -join "`n")
    })
}

$rootPrefix = $root.TrimEnd('\') + '\'
Assert-True $workRoot.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase) `
    'Acceptance workspace escaped the repository root.'
Assert-True (Test-Path -LiteralPath $productionSetup -PathType Leaf) `
    "Production Setup is missing: $productionSetup"
Assert-True (Test-Path -LiteralPath (Join-Path $compilerRoot 'ISCC.exe') -PathType Leaf) `
    "Pinned Inno compiler is missing: $compilerRoot"

if (Test-Path -LiteralPath $workRoot) {
    Remove-Item -LiteralPath $workRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $workRoot -Force | Out-Null

Invoke-StaticCheck -Name 'sixteen-locale mapping, aliases, fallback, provenance, and generated files' `
    -Script (Join-Path $root 'tools\i18n\test_inno_languages.py') `
    -Arguments @('--compiler-root', $compilerRoot, '--require-compiler')
Invoke-StaticCheck -Name 'all GameHQ CustomMessages, placeholders, tokens, consumers, and English classification' `
    -Script (Join-Path $root 'tools\i18n\test_inno_custom_messages.py')
Invoke-StaticCheck -Name 'sixteen semantic bootstrap mappings and one-shot consumer contract' `
    -Script (Join-Path $root 'tools\i18n\test_inno_bootstrap.py')

Write-Host '[installer-language-acceptance] RUNTIME: isolated sixteen-locale bootstrap matrix'
& (Join-Path $PSScriptRoot 'test-installer-bootstrap.ps1')
$bootstrapReportPath = Join-Path $root 'out\installer-bootstrap\report.json'
Assert-True (Test-Path -LiteralPath $bootstrapReportPath -PathType Leaf) `
    'Bootstrap runtime report was not produced.'
$bootstrapReport = Get-Content -LiteralPath $bootstrapReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-True (@($bootstrapReport.mappings).Count -eq 16) `
    'Bootstrap runtime report does not contain all sixteen mappings.'

Write-Host '[installer-language-acceptance] RUNTIME: bounded multilingual install, upgrade, failure, and uninstall matrix'
& (Join-Path $PSScriptRoot 'test-installer-regression.ps1') -SetupPath $productionSetup
$regressionReportPath = Join-Path $root 'out\installer-regression\report.json'
Assert-True (Test-Path -LiteralPath $regressionReportPath -PathType Leaf) `
    'Installer regression runtime report was not produced.'
$regressionReport = Get-Content -LiteralPath $regressionReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
$expectedRuntimeLocales = @('en-US', 'pl-PL', 'zh-Hans', 'zh-Hant', 'ru-RU', 'th-TH', 'es-419', 'de-DE')
$actualRuntimeLocales = @($regressionReport.critical_languages.locale)
Assert-True (($actualRuntimeLocales -join '|') -ceq ($expectedRuntimeLocales -join '|')) `
    "Runtime locale matrix differs (expected $($expectedRuntimeLocales -join ', '); got $($actualRuntimeLocales -join ', '))."
Assert-True ([string]$regressionReport.previous_language_reuse -ceq 'german') `
    'Runtime report lacks previous-language reuse evidence.'

$setupHash = (Get-FileHash -LiteralPath $productionSetup -Algorithm SHA256).Hash
$report = [ordered]@{
    schema_version = 1
    compiler_version = $toolchain.Version
    compiler_architecture = $toolchain.Architecture
    setup = [ordered]@{
        path = $productionSetup
        sha256 = $setupHash
    }
    static = @($staticChecks)
    runtime = [ordered]@{
        exhaustive_bootstrap_mappings = @($bootstrapReport.mappings).Count
        bootstrap_checks_passed = $bootstrapReport.checks_passed
        representative_locales = $actualRuntimeLocales
        regression_checks_passed = $regressionReport.checks_passed
        previous_language_reuse = $regressionReport.previous_language_reuse
    }
}
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Host "[installer-language-acceptance] ALL CHECKS PASSED"
Write-Host "[installer-language-acceptance] Setup SHA-256: $setupHash"
Write-Host "[installer-language-acceptance] report: $reportPath"
