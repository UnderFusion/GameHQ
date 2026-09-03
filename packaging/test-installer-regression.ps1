[CmdletBinding()]
param(
    [string]$SetupPath = ''
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$identity = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'distribution-identity.psd1')
$toolchain = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'inno-toolchain.psd1')
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
$compiler = Join-Path $root "tools\InnoSetup\$($toolchain.Version)\ISCC.exe"
$payloadRoot = Join-Path $root 'dist\.program-payload'
$workRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'out\installer-regression'))
$installRoot = Join-Path $workRoot 'install'
$outputRoot = Join-Path $workRoot 'output'
$logRoot = Join-Path $workRoot 'logs'
$reportPath = Join-Path $workRoot 'report.json'
$testAppId = '{B8C2B13D-130D-4DFD-B451-CF6C9FA5D956}'
$testProductSubKey = 'Software\underfusion\GameHQ-InstallerRegression'
$testAppPathSubKey = 'Software\Microsoft\Windows\CurrentVersion\App Paths\GameHQ-InstallerRegression.exe'
$testRunSubKey = 'Software\Microsoft\Windows\CurrentVersion\Run'
$testRunValue = 'GameHQ-InstallerRegression'
$testAppMutex = 'Local\GameHQInstallerRegressionApplicationActive'
$testUpdaterMutex = 'Local\GameHQInstallerRegressionUpdaterActive'
$testUninstallSubKey = "Software\Microsoft\Windows\CurrentVersion\Uninstall\$($testAppId)_is1"
$passes = [System.Collections.Generic.List[string]]::new()
$activeMutexJob = $null
$activeMutexName = $null
$activeMutexStop = $null

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
    Write-Host "[installer-regression] PASS: $Message"
}

function Get-RegistryValue {
    param([string]$SubKey, [string]$Name)
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($SubKey)
    if ($null -eq $key) { return $null }
    try { return $key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
    finally { $key.Dispose() }
}

function Get-RegistrySnapshot {
    param([string]$SubKey)
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($SubKey)
    if ($null -eq $key) { return '<absent>' }
    try {
        $values = [ordered]@{}
        foreach ($name in @($key.GetValueNames() | Sort-Object)) {
            $values[$name] = [string]$key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        }
        return ($values | ConvertTo-Json -Compress)
    }
    finally { $key.Dispose() }
}

function Remove-TestRegistry {
    foreach ($path in @(
        'HKCU:\Software\underfusion\GameHQ-InstallerRegression',
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\GameHQ-InstallerRegression.exe',
        "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$($testAppId)_is1"
    )) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
    Remove-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
        -Name $testRunValue -ErrorAction SilentlyContinue
}

function Invoke-Program {
    param([string]$FilePath, [string[]]$Arguments)
    $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
    return $process.ExitCode
}

function Invoke-TestSetup {
    param([string]$Language = '', [string]$Name, [int]$ExpectedExit)
    $log = Join-Path $logRoot "$Name.log"
    $arguments = @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/NOICONS',
        "/DIR=$installRoot", "/LOG=$log"
    )
    if (-not [string]::IsNullOrWhiteSpace($Language)) {
        $arguments += "/LANG=$Language"
    }
    $exitCode = Invoke-Program -FilePath $script:fixtureSetup -Arguments $arguments
    Assert-Equal $exitCode $ExpectedExit "$Name Setup exit code"
}

function Invoke-TestUninstall {
    param([string]$Name, [switch]$ExpectRefusal)
    $uninstaller = Join-Path $installRoot 'unins000.exe'
    Assert-True (Test-Path -LiteralPath $uninstaller -PathType Leaf) "$Name requires the installed uninstaller."
    $log = Join-Path $logRoot "$Name.log"
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$log")
    $exitCode = Invoke-Program -FilePath $uninstaller -Arguments $arguments
    if ($ExpectRefusal) {
        Assert-True ($exitCode -ne 0) "$Name Uninstall unexpectedly reported success."
    }
    else {
        Assert-Equal $exitCode 0 "$Name Uninstall exit code"
    }
}

function Set-MaintenanceState {
    param([string]$Phase, [int]$AgeSeconds = 0)
    $updateRoot = Join-Path $installRoot '.update'
    New-Item -ItemType Directory -Path $updateRoot -Force | Out-Null
    $marker = Join-Path $updateRoot 'maintenance.lock'
    $phasePath = Join-Path $updateRoot 'transaction.phase'
    [System.IO.File]::WriteAllText($marker, 'installer-regression')
    [System.IO.File]::WriteAllText($phasePath, $Phase)
    if ($AgeSeconds -gt 0) {
        (Get-Item -LiteralPath $marker).LastWriteTimeUtc = [DateTime]::UtcNow.AddSeconds(-$AgeSeconds)
    }
}

function Start-TestMutex {
    param([string]$Name, [string]$Label)
    $ready = Join-Path $workRoot "$Label.ready"
    $stop = Join-Path $workRoot "$Label.stop"
    Remove-Item -LiteralPath $ready, $stop -Force -ErrorAction SilentlyContinue
    $job = Start-Job -ScriptBlock {
        param($MutexName, $ReadyPath, $StopPath)
        $created = $false
        $mutex = [System.Threading.Mutex]::new($true, $MutexName, [ref]$created)
        if (-not $created) { throw "Could not acquire test mutex $MutexName." }
        try {
            [System.IO.File]::WriteAllText($ReadyPath, 'ready')
            while (-not (Test-Path -LiteralPath $StopPath)) { Start-Sleep -Milliseconds 50 }
        }
        finally {
            $mutex.ReleaseMutex()
            $mutex.Dispose()
        }
    } -ArgumentList $Name, $ready, $stop
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath $ready)) {
        if ($job.State -in @('Completed', 'Failed', 'Stopped')) {
            $details = (Receive-Job -Job $job -ErrorAction SilentlyContinue | Out-String).Trim()
            Remove-Job -Job $job -Force
            throw "Test mutex job ended before readiness: $details"
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            Stop-Job -Job $job -ErrorAction SilentlyContinue
            Remove-Job -Job $job -Force
            throw "Timed out acquiring test mutex $Name."
        }
        Start-Sleep -Milliseconds 50
    }
    $script:activeMutexJob = $job
    $script:activeMutexName = $Name
    $script:activeMutexStop = $stop
}

function Stop-TestMutex {
    if ($null -eq $script:activeMutexJob) { return }
    [System.IO.File]::WriteAllText($script:activeMutexStop, 'stop')
    Wait-Job -Job $script:activeMutexJob -Timeout 15 | Out-Null
    if ($script:activeMutexJob.State -ne 'Completed') {
        Stop-Job -Job $script:activeMutexJob -ErrorAction SilentlyContinue
        throw 'Test mutex job did not stop cleanly.'
    }
    Receive-Job -Job $script:activeMutexJob -ErrorAction Stop | Out-Null
    Remove-Job -Job $script:activeMutexJob -Force
    $script:activeMutexJob = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ($true) {
        try {
            $probe = [System.Threading.Mutex]::OpenExisting($script:activeMutexName)
            $probe.Dispose()
        }
        catch [System.Threading.WaitHandleCannotBeOpenedException] {
            break
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "Timed out waiting for test mutex $($script:activeMutexName) to disappear."
        }
        Start-Sleep -Milliseconds 50
    }
    $script:activeMutexName = $null
    $script:activeMutexStop = $null
    # Antivirus and the just-finished Setup process can briefly retain an image
    # handle after the named mutex is gone. Let the production file-lock guard
    # settle so the following scenario tests its intended blocker.
    Start-Sleep -Milliseconds 1000
}

$rootPrefix = $root.TrimEnd('\') + '\'
Assert-True $workRoot.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase) 'Regression workspace escaped the repository root.'
Assert-True (Test-Path -LiteralPath $compiler -PathType Leaf) "Pinned compiler is missing: $compiler"
Assert-True (Test-Path -LiteralPath $payloadRoot -PathType Container) "Neutral payload is missing: $payloadRoot"

$realKeys = @(
    'Software\underfusion\GameHQ',
    'Software\Microsoft\Windows\CurrentVersion\App Paths\GameHQ.exe',
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\$($identity.InnoAppId)_is1",
    'Software\Microsoft\Windows\CurrentVersion\Run'
)
$realBefore = [ordered]@{}
foreach ($key in $realKeys) { $realBefore[$key] = Get-RegistrySnapshot $key }

if (Test-Path -LiteralPath $workRoot) { Remove-Item -LiteralPath $workRoot -Recurse -Force }
New-Item -ItemType Directory -Path $outputRoot, $logRoot -Force | Out-Null
Remove-TestRegistry

try {
    $setupName = "GameHQ-$version-installer-regression.exe"
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
        "/DRunRegistryKey=$testRunSubKey",
        "/DRunRegistryValue=$testRunValue",
        "/DApplicationMutexValue=$testAppMutex",
        "/DUpdaterMutexValue=$testUpdaterMutex",
        (Join-Path $PSScriptRoot 'GameHQ.iss')
    )
    & $compiler @compileArguments
    if ($LASTEXITCODE -ne 0) { throw "Isolated installer fixture compilation failed ($LASTEXITCODE)." }
    $script:fixtureSetup = Join-Path $outputRoot $setupName
    Assert-True (Test-Path -LiteralPath $script:fixtureSetup -PathType Leaf) 'Fixture Setup was not created.'
    $fixtureVersion = (Get-Item -LiteralPath $script:fixtureSetup).VersionInfo
    Assert-Equal $fixtureVersion.ProductName.Trim() 'GameHQ' 'Setup product name'
    Assert-Equal $fixtureVersion.ProductVersion.Trim() $version 'Setup product version'
    Assert-Equal $fixtureVersion.FileVersion.Trim() $versionInfo 'Setup file version'
    Assert-Equal $fixtureVersion.CompanyName.Trim() $identity.Publisher 'Setup publisher'
    Add-Pass "Inno $($toolchain.Version) x64 compiled the isolated fixture with production defaults preserved"

    if ($SetupPath) {
        $productionSetup = [System.IO.Path]::GetFullPath($SetupPath)
        Assert-True (Test-Path -LiteralPath $productionSetup -PathType Leaf) "Production Setup is missing: $productionSetup"
        $productionInfo = (Get-Item -LiteralPath $productionSetup).VersionInfo
        Assert-Equal $productionInfo.ProductName.Trim() 'GameHQ' 'Production Setup product name'
        Assert-Equal $productionInfo.ProductVersion.Trim() $version 'Production Setup product version'
        Assert-Equal $productionInfo.FileVersion.Trim() $versionInfo 'Production Setup file version'
        Assert-Equal $productionInfo.CompanyName.Trim() $identity.Publisher 'Production Setup publisher'
        Add-Pass 'production Setup metadata matches the product identity and version'
    }

    $criticalLanguages = @(
        @{ Name = 'english'; Label = 'fresh-en-US'; Locale = 'en-US' },
        @{ Name = 'polish'; Label = 'upgrade-pl-PL'; Locale = 'pl-PL' },
        @{ Name = 'chinesesimp'; Label = 'upgrade-zh-Hans'; Locale = 'zh-Hans' },
        @{ Name = 'chinesetrad'; Label = 'upgrade-zh-Hant'; Locale = 'zh-Hant' },
        @{ Name = 'russian'; Label = 'upgrade-ru-RU'; Locale = 'ru-RU' },
        @{ Name = 'thai'; Label = 'upgrade-th-TH'; Locale = 'th-TH' },
        @{ Name = 'spanishlatinamerica'; Label = 'upgrade-es-419'; Locale = 'es-419' },
        @{ Name = 'german'; Label = 'upgrade-de-DE-long-text'; Locale = 'de-DE' }
    )
    foreach ($language in $criticalLanguages) {
        Invoke-TestSetup -Language $language.Name -Name $language.Label -ExpectedExit 0
        Assert-Equal (Get-RegistryValue $testUninstallSubKey 'Inno Setup: Language') $language.Name "$($language.Label) selected language"
        Assert-Equal (Get-RegistryValue $testUninstallSubKey 'DisplayVersion') $version "$($language.Label) display version"
        Assert-True ([string](Get-RegistryValue $testUninstallSubKey 'Inno Setup: Setup Version')).StartsWith($toolchain.Version) "$($language.Label) did not record Inno $($toolchain.Version)."
        Add-Pass "$($language.Label) installed or upgraded and retained its distinct installer language"
        if ($language.Label -eq 'fresh-en-US') {
            Assert-True (Test-Path -LiteralPath (Join-Path $installRoot 'GameHQ.exe') -PathType Leaf) 'Fresh install omitted the launcher.'
            Assert-True (Test-Path -LiteralPath (Join-Path $installRoot 'GameHQUpdater.exe') -PathType Leaf) 'Fresh install omitted the updater.'
            Assert-True (Test-Path -LiteralPath (Join-Path $installRoot 'app\GameHQ.exe') -PathType Leaf) 'Fresh install omitted the application.'
            $sentinel = Join-Path $installRoot 'gamehq-data\installer-regression-sentinel.txt'
            New-Item -ItemType Directory -Path (Split-Path -Parent $sentinel) -Force | Out-Null
            [System.IO.File]::WriteAllText($sentinel, 'preserve')
            Add-Pass 'fresh install created the expected launcher, updater, and application layout'
        }
    }

    if ($SetupPath) {
        $localizedAssetReport = Join-Path $workRoot 'installed-localization-assets.json'
        $localizedAssetProbe = Start-Process -FilePath (Join-Path $installRoot 'app\GameHQ.exe') `
            -ArgumentList @('--localization-assets-self-test', ('"' + $localizedAssetReport + '"')) `
            -WindowStyle Hidden -Wait -PassThru
        Assert-Equal $localizedAssetProbe.ExitCode 0 'Installed localization asset probe exit code'
        Assert-True (Test-Path -LiteralPath $localizedAssetReport -PathType Leaf) `
            'Installed localization asset probe did not produce its report.'
        $localizedAssets = Get-Content -LiteralPath $localizedAssetReport -Raw -Encoding UTF8 | ConvertFrom-Json
        Assert-Equal $localizedAssets.production_locale_count 16 'Installed production locale count'
        Assert-Equal $localizedAssets.catalog_count 16 'Installed catalog count'
        Assert-Equal $localizedAssets.release_note_bundle_count 16 'Installed release-note bundle count'
        Add-Pass 'production Setup installed the same verified sixteen-locale application resources'
    }

    Invoke-TestSetup -Name 'upgrade-previous-language-reuse' -ExpectedExit 0
    Assert-Equal (Get-RegistryValue $testUninstallSubKey 'Inno Setup: Language') 'german' `
        'Upgrade without /LANG did not reuse the previous installer language'
    Add-Pass 'upgrade without explicit selection reuses the previous installer language'

    Assert-Equal (Get-RegistryValue $testProductSubKey 'InstallLocation') $installRoot 'Product InstallLocation'
    Assert-Equal (Get-RegistryValue $testProductSubKey 'Version') $version 'Product Version'
    Assert-Equal (Get-RegistryValue $testAppPathSubKey '') (Join-Path $installRoot 'GameHQ.exe') 'App Paths executable'
    Assert-Equal (Get-RegistryValue $testAppPathSubKey 'Path') $installRoot 'App Paths directory'
    Assert-Equal (Get-RegistryValue $testUninstallSubKey 'Publisher') $identity.Publisher 'Uninstall publisher'
    Assert-Equal (Get-RegistryValue $testUninstallSubKey 'DisplayName') "GameHQ $version" 'Uninstall display name'
    Add-Pass 'installed registry and uninstall metadata match the canonical product identity'

    Start-TestMutex -Name $testAppMutex -Label 'setup-app-mutex'
    try { Invoke-TestSetup -Language 'english' -Name 'setup-app-mutex' -ExpectedExit 20 }
    finally { Stop-TestMutex }
    Add-Pass 'silent Setup preserves reserved exit 20 while the application mutex is active'

    Set-MaintenanceState -Phase 'installing' -AgeSeconds 360
    Start-TestMutex -Name $testUpdaterMutex -Label 'setup-updater-mutex'
    try { Invoke-TestSetup -Language 'english' -Name 'setup-updater-active' -ExpectedExit 21 }
    finally { Stop-TestMutex }
    Add-Pass 'silent Setup preserves reserved exit 21 while the updater mutex is active'

    Set-MaintenanceState -Phase 'installing' -AgeSeconds 360
    Invoke-TestSetup -Language 'english' -Name 'setup-stale-recovery' -ExpectedExit 21
    Assert-True (Test-Path -LiteralPath (Join-Path $installRoot '.update\maintenance.lock')) 'Setup removed the stale maintenance marker.'
    Assert-Equal ([System.IO.File]::ReadAllText((Join-Path $installRoot '.update\transaction.phase'))) 'installing' 'Setup changed the stale transaction phase'
    Add-Pass 'silent Setup detects stale recovery state and preserves its evidence'

    Set-MaintenanceState -Phase 'rolled_back'
    Invoke-TestSetup -Language 'english' -Name 'setup-rolled-back' -ExpectedExit 0
    Assert-True (Test-Path -LiteralPath (Join-Path $installRoot '.update\maintenance.lock')) 'Setup removed the rolled-back marker.'
    Add-Pass 'rolled_back is a terminal recovery phase and does not block Setup'

    Start-TestMutex -Name $testAppMutex -Label 'uninstall-app-mutex'
    try { Invoke-TestUninstall -Name 'uninstall-app-mutex' -ExpectRefusal }
    finally { Stop-TestMutex }
    Add-Pass 'silent Uninstall returns nonzero while the application mutex is active'

    Set-MaintenanceState -Phase 'installing' -AgeSeconds 360
    Start-TestMutex -Name $testUpdaterMutex -Label 'uninstall-updater-mutex'
    try { Invoke-TestUninstall -Name 'uninstall-updater-active' -ExpectRefusal }
    finally { Stop-TestMutex }
    Add-Pass 'silent Uninstall returns nonzero while the updater mutex is active'

    Set-MaintenanceState -Phase 'installing' -AgeSeconds 360
    Invoke-TestUninstall -Name 'uninstall-stale-recovery' -ExpectRefusal
    Assert-True (Test-Path -LiteralPath (Join-Path $installRoot '.update\maintenance.lock')) 'Uninstall removed the stale maintenance marker.'
    Add-Pass 'silent Uninstall detects stale recovery state and preserves its evidence'

    Set-MaintenanceState -Phase 'healthy'
    Invoke-TestUninstall -Name 'uninstall-healthy'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $installRoot 'GameHQ.exe'))) 'Uninstall left installed program files behind.'
    Assert-True (Test-Path -LiteralPath (Join-Path $installRoot 'gamehq-data\installer-regression-sentinel.txt')) 'Uninstall removed user data.'
    Assert-True ($null -eq (Get-RegistryValue $testProductSubKey 'InstallLocation')) 'Uninstall left the test product registry value.'
    Assert-True ($null -eq (Get-RegistryValue $testAppPathSubKey '')) 'Uninstall left the test App Paths value.'
    Assert-True ($null -eq (Get-RegistryValue $testUninstallSubKey 'DisplayName')) 'Uninstall left its registration behind.'
    Add-Pass 'terminal healthy state permits uninstall, which removes program integration and preserves user data'

    foreach ($key in $realKeys) {
        Assert-Equal (Get-RegistrySnapshot $key) $realBefore[$key] "Real GameHQ registry state changed at $key"
    }
    Add-Pass 'the isolated regression left the real GameHQ installation metadata unchanged'

    $report = [ordered]@{
        schema_version = 1
        compiler_version = $toolchain.Version
        compiler_architecture = $toolchain.Architecture
        application_version = $version
        critical_languages = @($criticalLanguages | ForEach-Object {
            [ordered]@{ locale = $_.Locale; installer_language = $_.Name; scenario = $_.Label }
        })
        previous_language_reuse = 'german'
        checks_passed = $passes.Count
        checks = @($passes)
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    Write-Host "[installer-regression] ALL $($passes.Count) CHECKS PASSED"
    Write-Host "[installer-regression] report: $reportPath"
}
finally {
    if ($null -ne $activeMutexJob) {
        try { Stop-TestMutex } catch { Write-Warning $_ }
    }
    Remove-TestRegistry
}
