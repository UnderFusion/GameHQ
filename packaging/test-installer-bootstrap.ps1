[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$identity = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'distribution-identity.psd1')
$toolchain = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'inno-toolchain.psd1')
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
$compiler = Join-Path $root "tools\InnoSetup\$($toolchain.Version)\ISCC.exe"
$workRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'out\installer-bootstrap'))
$installRoot = Join-Path $workRoot 'install'
$payloadRoot = Join-Path $workRoot 'payload'
$outputRoot = Join-Path $workRoot 'output'
$logRoot = Join-Path $workRoot 'logs'
$reportPath = Join-Path $workRoot 'report.json'
$testAppId = '{D90CE525-9F85-486A-AD93-E23CBBDB5E13}'
$testProductSubKey = 'Software\underfusion\GameHQ-InstallerBootstrap'
$testAppPathSubKey = 'Software\Microsoft\Windows\CurrentVersion\App Paths\GameHQ-InstallerBootstrap.exe'
$testRunValue = 'GameHQ-InstallerBootstrap'
$testUninstallSubKey = "Software\Microsoft\Windows\CurrentVersion\Uninstall\$($testAppId)_is1"
$profileRelativePath = 'GameHQ-InstallerBootstrap\config.json'
$appDataRoot = [Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)
$profileRoot = [System.IO.Path]::GetFullPath((Join-Path $appDataRoot 'GameHQ-InstallerBootstrap'))
$profileConfig = Join-Path $profileRoot 'config.json'
$passes = [System.Collections.Generic.List[string]]::new()
$scenarioNumber = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Equal {
    param($Actual, $Expected, [string]$Message)
    if ([string]$Actual -cne [string]$Expected) {
        throw "$Message (expected '$Expected', got '$Actual')."
    }
}

function Add-Pass {
    param([string]$Message)
    $passes.Add($Message)
    Write-Host "[installer-bootstrap] PASS: $Message"
}

function Get-RegistryValue {
    param([string]$SubKey, [string]$Name)
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($SubKey)
    if ($null -eq $key) { return $null }
    try {
        return $key.GetValue(
            $Name, $null,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    }
    finally { $key.Dispose() }
}

function Set-RegistryValue {
    param([string]$SubKey, [string]$Name, [string]$Value)
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($SubKey)
    try { $key.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]::String) }
    finally { $key.Dispose() }
}

function Remove-RegistryValue {
    param([string]$SubKey, [string]$Name)
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($SubKey, $true)
    if ($null -eq $key) { return }
    try { $key.DeleteValue($Name, $false) }
    finally { $key.Dispose() }
}

function Get-RegistrySnapshot {
    param([string]$SubKey)
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($SubKey)
    if ($null -eq $key) { return '<absent>' }
    try {
        $values = [ordered]@{}
        foreach ($name in @($key.GetValueNames() | Sort-Object)) {
            $values[$name] = [string]$key.GetValue(
                $name, $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        }
        return ($values | ConvertTo-Json -Compress)
    }
    finally { $key.Dispose() }
}

function Remove-TestRegistry {
    foreach ($path in @(
        'HKCU:\Software\underfusion\GameHQ-InstallerBootstrap',
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\GameHQ-InstallerBootstrap.exe',
        "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$($testAppId)_is1"
    )) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
    Remove-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
        -Name $testRunValue -ErrorAction SilentlyContinue
}

function Remove-ScenarioState {
    Remove-TestRegistry
    if (Test-Path -LiteralPath $installRoot) {
        Remove-Item -LiteralPath $installRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $profileRoot) {
        Remove-Item -LiteralPath $profileRoot -Recurse -Force
    }
}

function Invoke-Program {
    param([string]$FilePath, [string[]]$Arguments)
    $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments `
        -WindowStyle Hidden -Wait -PassThru
    return $process.ExitCode
}

function Invoke-TestSetup {
    param([string]$Language, [string]$Label)
    $script:scenarioNumber++
    $log = Join-Path $logRoot ('{0:D2}-{1}.log' -f $script:scenarioNumber, $Label)
    $arguments = @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/NOICONS',
        "/DIR=$installRoot", "/LANG=$Language", "/LOG=$log"
    )
    Assert-Equal (Invoke-Program -FilePath $script:fixtureSetup -Arguments $arguments) 0 `
        "$Label Setup exit code"
}

function Invoke-TestUninstall {
    param([string]$Label)
    $uninstaller = Join-Path $installRoot 'unins000.exe'
    Assert-True (Test-Path -LiteralPath $uninstaller -PathType Leaf) `
        "$Label requires the installed uninstaller."
    $script:scenarioNumber++
    $log = Join-Path $logRoot ('{0:D2}-{1}.log' -f $script:scenarioNumber, $Label)
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$log")
    Assert-Equal (Invoke-Program -FilePath $uninstaller -Arguments $arguments) 0 `
        "$Label Uninstall exit code"
}

function Write-TestConfig {
    param([string]$Json)
    New-Item -ItemType Directory -Path $profileRoot -Force | Out-Null
    [System.IO.File]::WriteAllText($profileConfig, $Json)
}

$rootPrefix = $root.TrimEnd('\') + '\'
$appDataPrefix = [System.IO.Path]::GetFullPath($appDataRoot).TrimEnd('\') + '\'
Assert-True ($workRoot.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) `
    'Bootstrap workspace escaped the repository root.'
Assert-True ($profileRoot.StartsWith($appDataPrefix, [System.StringComparison]::OrdinalIgnoreCase)) `
    'Bootstrap test profile escaped the user application-data root.'
Assert-True (Test-Path -LiteralPath $compiler -PathType Leaf) `
    "Pinned compiler is missing: $compiler"

$realKeys = @(
    'Software\underfusion\GameHQ',
    'Software\Microsoft\Windows\CurrentVersion\App Paths\GameHQ.exe',
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\$($identity.InnoAppId)_is1"
)
$realBefore = [ordered]@{}
foreach ($key in $realKeys) { $realBefore[$key] = Get-RegistrySnapshot $key }
$realConfig = Join-Path $appDataRoot 'GameHQ\config.json'
$realConfigBefore = if (Test-Path -LiteralPath $realConfig -PathType Leaf) {
    (Get-FileHash -LiteralPath $realConfig -Algorithm SHA256).Hash
} else { '<absent>' }

if (Test-Path -LiteralPath $workRoot) {
    Remove-Item -LiteralPath $workRoot -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $payloadRoot 'app'), $outputRoot, $logRoot `
    -Force | Out-Null
foreach ($file in @('GameHQ.exe', 'GameHQUpdater.exe', 'app\GameHQ.exe')) {
    [System.IO.File]::WriteAllText((Join-Path $payloadRoot $file), 'installer-bootstrap-fixture')
}
Remove-ScenarioState

try {
    $setupName = "GameHQ-$version-installer-bootstrap.exe"
    $setupBase = [System.IO.Path]::GetFileNameWithoutExtension($setupName)
    $versionParts = $version.Split('.')
    $versionInfo = "$($versionParts[0]).$($versionParts[1]).$($versionParts[2]).0"
    $compileArguments = @(
        '/Qp',
        "/DAppVersion=$version",
        "/DAppVersionInfo=$versionInfo",
        "/DInstallerAppId=$testAppId",
        "/DPayloadRoot=$payloadRoot",
        "/DReleaseOutput=$outputRoot",
        "/DSetupBaseName=$setupBase",
        "/DProductRegistryKey=$testProductSubKey",
        "/DAppPathRegistryKey=$testAppPathSubKey",
        '/DRunRegistryKey=Software\Microsoft\Windows\CurrentVersion\Run',
        "/DRunRegistryValue=$testRunValue",
        '/DApplicationMutexValue=Local\GameHQInstallerBootstrapApplicationActive',
        '/DUpdaterMutexValue=Local\GameHQInstallerBootstrapUpdaterActive',
        "/DBootstrapProfileRelativePath=$profileRelativePath",
        (Join-Path $PSScriptRoot 'GameHQ.iss')
    )
    & $compiler @compileArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Isolated bootstrap fixture compilation failed ($LASTEXITCODE)."
    }
    $script:fixtureSetup = Join-Path $outputRoot $setupName
    Assert-True (Test-Path -LiteralPath $script:fixtureSetup -PathType Leaf) `
        'Bootstrap fixture Setup was not created.'
    Add-Pass "Inno $($toolchain.Version) x64 compiled the isolated bootstrap fixture"

    $manifest = Get-Content -LiteralPath (Join-Path $root 'i18n\locales.json') `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    $locales = @($manifest.locales | Where-Object {
        $_.state -eq 'enabled' -and $_.tier -eq 1
    } | Sort-Object inno_order)
    Assert-Equal $locales.Count 16 'Production installer locale count'
    foreach ($locale in $locales) {
        Remove-ScenarioState
        Invoke-TestSetup -Language $locale.inno_language -Label "fresh-$($locale.tag)"
        Assert-Equal (Get-RegistryValue $testProductSubKey 'BootstrapLanguage') `
            $locale.inno_app_locale "$($locale.tag) semantic handoff"
        Assert-Equal (Get-RegistryValue $testProductSubKey 'BootstrapLanguageOffered') 1 `
            "$($locale.tag) offered marker"
        Assert-Equal (Get-RegistryValue $testUninstallSubKey 'Inno Setup: Language') `
            $locale.inno_language "$($locale.tag) selected Setup language"
        Add-Pass "fresh silent Setup hands $($locale.inno_language) to $($locale.inno_app_locale)"
    }

    Remove-ScenarioState
    Write-TestConfig '{"ui.language":"pl-PL"}'
    Invoke-TestSetup -Language 'english' -Label 'existing-explicit-locale'
    Assert-True ($null -eq (Get-RegistryValue $testProductSubKey 'BootstrapLanguage')) `
        'Existing explicit locale received a bootstrap.'
    Assert-Equal ([System.IO.File]::ReadAllText($profileConfig)) '{"ui.language":"pl-PL"}' `
        'Setup changed the existing locale config'
    Add-Pass 'an existing explicit application locale suppresses the bootstrap unchanged'

    Remove-ScenarioState
    Write-TestConfig '{"ui.language":"system"}'
    Invoke-TestSetup -Language 'polish' -Label 'existing-system-locale'
    Assert-True ($null -eq (Get-RegistryValue $testProductSubKey 'BootstrapLanguage')) `
        'Explicit system preference received a bootstrap.'
    Assert-Equal ([System.IO.File]::ReadAllText($profileConfig)) '{"ui.language":"system"}' `
        'Setup changed the explicit system config'
    Add-Pass 'an explicit system preference suppresses the bootstrap unchanged'

    Remove-ScenarioState
    Invoke-TestSetup -Language 'polish' -Label 'pending-bootstrap-first-install'
    Invoke-TestSetup -Language 'english' -Label 'pending-bootstrap-upgrade'
    Assert-Equal (Get-RegistryValue $testProductSubKey 'BootstrapLanguage') 'pl-PL' `
        'Upgrade replaced a pending one-shot bootstrap'
    Add-Pass 'upgrade preserves a pending first-launch bootstrap'

    Remove-RegistryValue -SubKey $testProductSubKey -Name 'BootstrapLanguage'
    Invoke-TestSetup -Language 'italian' -Label 'consumed-bootstrap-upgrade'
    Assert-True ($null -eq (Get-RegistryValue $testProductSubKey 'BootstrapLanguage')) `
        'Upgrade recreated a consumed bootstrap.'
    Add-Pass 'the offered marker prevents a consumed bootstrap from being recreated'

    Write-TestConfig '{"ui.language":"pl-PL"}'
    Invoke-TestUninstall -Label 'consumed-bootstrap-uninstall'
    Assert-Equal (Get-RegistryValue $testProductSubKey 'BootstrapLanguageOffered') 1 `
        'Uninstall removed the one-shot marker'
    Invoke-TestSetup -Language 'english' -Label 'consumed-bootstrap-reinstall'
    Assert-True ($null -eq (Get-RegistryValue $testProductSubKey 'BootstrapLanguage')) `
        'Reinstall recreated a consumed bootstrap.'
    Add-Pass 'uninstall and reinstall preserve the consumed one-shot state and user locale'

    Remove-ScenarioState
    Set-RegistryValue -SubKey $testProductSubKey -Name 'InstallLocation' -Value $installRoot
    Invoke-TestSetup -Language 'ukrainian' -Label 'pre-marker-upgrade'
    Assert-True ($null -eq (Get-RegistryValue $testProductSubKey 'BootstrapLanguage')) `
        'An existing pre-marker installation received a bootstrap.'
    Assert-Equal (Get-RegistryValue $testProductSubKey 'BootstrapLanguageOffered') 1 `
        'Existing installation did not receive the suppression marker'
    Add-Pass 'an existing pre-feature install is marked without forcing a language'

    Remove-ScenarioState
    Set-RegistryValue -SubKey $testProductSubKey -Name 'BootstrapLanguage' `
        -Value 'not-a-locale'
    Invoke-TestSetup -Language 'german' -Label 'stale-bootstrap'
    Assert-True ($null -eq (Get-RegistryValue $testProductSubKey 'BootstrapLanguage')) `
        'Setup retained stale bootstrap state.'
    Assert-Equal (Get-RegistryValue $testProductSubKey 'BootstrapLanguageOffered') 1 `
        'Stale bootstrap did not receive the suppression marker'
    Add-Pass 'stale or malformed bootstrap state is discarded without replacement'

    Remove-ScenarioState
    Write-TestConfig '{'
    Invoke-TestSetup -Language 'thai' -Label 'malformed-existing-profile'
    Assert-True ($null -eq (Get-RegistryValue $testProductSubKey 'BootstrapLanguage')) `
        'A malformed existing profile received a bootstrap.'
    Add-Pass 'a malformed existing profile fails safely without installer preference changes'

    Remove-ScenarioState
    New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $installRoot 'portable.flag'), 'portable')
    Invoke-TestSetup -Language 'french' -Label 'portable-profile'
    Assert-True ($null -eq (Get-RegistryValue $testProductSubKey 'BootstrapLanguage')) `
        'Portable target received a BootstrapLanguage value.'
    Assert-True ($null -eq (Get-RegistryValue $testProductSubKey 'BootstrapLanguageOffered')) `
        'Portable target received bootstrap lifecycle state.'
    Add-Pass 'portable targets neither receive nor mutate installer bootstrap state'

    foreach ($key in $realKeys) {
        Assert-Equal (Get-RegistrySnapshot $key) $realBefore[$key] `
            "Real GameHQ registry state changed at $key"
    }
    $realConfigAfter = if (Test-Path -LiteralPath $realConfig -PathType Leaf) {
        (Get-FileHash -LiteralPath $realConfig -Algorithm SHA256).Hash
    } else { '<absent>' }
    Assert-Equal $realConfigAfter $realConfigBefore 'Real GameHQ config changed'
    Add-Pass 'the isolated bootstrap matrix left the real GameHQ profile unchanged'

    $report = [ordered]@{
        schema_version = 1
        compiler_version = $toolchain.Version
        mappings = @($locales | ForEach-Object {
            [ordered]@{ installer_language = $_.inno_language; app_locale = $_.inno_app_locale }
        })
        checks_passed = $passes.Count
        checks = @($passes)
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    Write-Host "[installer-bootstrap] ALL $($passes.Count) CHECKS PASSED"
    Write-Host "[installer-bootstrap] report: $reportPath"
}
finally {
    Remove-ScenarioState
}
