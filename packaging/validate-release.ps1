[CmdletBinding()]
param(
    [string]$ReleaseDirectory = 'dist\releases',
    [string]$GitTag = '',
    [Parameter(Mandatory = $true)]
    [ValidateSet('unsigned-beta', 'signed')]
    [string]$TrustMode,
    [ValidateSet('none', 'test', 'production')]
    [string]$ManifestMode = 'none',
    [string]$NinjaPath = '',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
& (Join-Path $PSScriptRoot 'assert-privacy.ps1') -ReleaseDirectory $ReleaseDirectory
& (Join-Path $PSScriptRoot 'assert-license-compliance.ps1')
& (Join-Path $PSScriptRoot 'test-release-note-assets.ps1')
$releaseRoot = if ([System.IO.Path]::IsPathRooted($ReleaseDirectory)) {
    [System.IO.Path]::GetFullPath($ReleaseDirectory)
} else { [System.IO.Path]::GetFullPath((Join-Path $root $ReleaseDirectory)) }
$releaseRoot = $releaseRoot.TrimEnd('\', '/')
$approvedRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'dist\releases')).TrimEnd('\', '/')
if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($releaseRoot, $approvedRoot)) {
    throw "Release validation is restricted to $approvedRoot"
}
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
$minimumUpdaterVersion = (Get-Content (Join-Path $PSScriptRoot 'minimum-updater-version.txt') -Raw).Trim()
$identity = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'distribution-identity.psd1')
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION is not valid semantic version text.' }
if ($minimumUpdaterVersion -notmatch '^\d+\.\d+\.\d+$' `
    -or [version]$minimumUpdaterVersion -gt [version]$version) {
    throw 'minimum-updater-version.txt must contain a valid version no newer than VERSION.'
}
if ($GitTag -and $GitTag.TrimStart('v') -ne $version) {
    throw "Git tag $GitTag does not match VERSION $version."
}
if ($GitTag -and $ManifestMode -eq 'test') {
    throw 'Test-key manifests are local/CI evidence only and can never validate a publishable Git tag.'
}

$portableName = $identity.ArtifactPatterns.Portable -f $version
$updateName = $identity.ArtifactPatterns.Update -f $version
$setupName = $identity.ArtifactPatterns.Setup -f $version
$sourceName = $identity.ArtifactPatterns.Source -f $version
$sourceChecksumName = $identity.ArtifactPatterns.SourceChecksum -f $version
$portableZip = Join-Path $releaseRoot $portableName
$updateZip = Join-Path $releaseRoot $updateName
$setup = Join-Path $releaseRoot $setupName
$sourceZip = Join-Path $releaseRoot $sourceName
$sourceChecksum = Join-Path $releaseRoot $sourceChecksumName
$checksum = "$updateZip.sha256"
$payloadRoot = Join-Path $root 'dist\.program-payload'
$gameInputToolchain = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'gameinput-toolchain.psd1')
$gameInputRuntimeRelative = 'app/GameInputRedist.dll'
foreach ($required in @($setup, $portableZip, $updateZip, $checksum, $sourceZip, $sourceChecksum)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing release artifact: $required" }
}
if ($ManifestMode -in @('test', 'production')) {
    foreach ($required in @('gamehq-release.json', 'gamehq-release.sig')) {
        if (-not (Test-Path -LiteralPath (Join-Path $releaseRoot $required) -PathType Leaf)) {
            throw "Missing $ManifestMode release-manifest artifact: $required"
        }
    }
    $tool = Join-Path $root 'tools\release-manifest\GameHQ.ReleaseManifest.Tool\GameHQ.ReleaseManifest.Tool.csproj'
    $verifyArguments = @(
        if ($ManifestMode -eq 'test') { 'verify-test' } else { 'verify-production' }
        '--release-dir', $releaseRoot,
        '--minimum-updater-version', $minimumUpdaterVersion
    )
    if ($ManifestMode -eq 'production') {
        $trust = Get-Content (Join-Path $PSScriptRoot 'release-trust.json') -Raw | ConvertFrom-Json
        $verifyArguments += @(
            '--key-id', [string]$trust.currentKey.keyId,
            '--public-key-base64', [string]$trust.currentKey.publicKeyBase64
        )
    }
    & dotnet run --project $tool --configuration Release --no-restore -- @verifyArguments
    if ($LASTEXITCODE -ne 0) { throw "$ManifestMode release manifest verification failed." }
} elseif ($TrustMode -eq 'signed') {
    throw 'Signed/Stable validation requires the production manifest mode, which is intentionally gated by t48.'
}
$setupInfo = (Get-Item -LiteralPath $setup).VersionInfo
if ($setupInfo.ProductVersion.Trim() -ne $version -or
    $setupInfo.FileVersion.Trim() -ne "$version.0" -or
    $setupInfo.CompanyName.Trim() -ne $identity.Publisher -or
    $setupInfo.ProductName.Trim() -ne $identity.ProductName) {
    throw 'Setup Windows metadata does not match the distribution identity and VERSION.'
}

$gamehqBuilt = @(
    $setup,
    (Join-Path $payloadRoot 'GameHQ.exe'),
    (Join-Path $payloadRoot 'GameHQUpdater.exe'),
    (Join-Path $payloadRoot 'app\GameHQ.exe')
)
$signatures = @($gamehqBuilt | ForEach-Object {
    $signature = Get-AuthenticodeSignature -FilePath $_
    [pscustomobject]@{ Path = $_; Status = [string]$signature.Status;
        Subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' } }
})
if ($TrustMode -eq 'unsigned-beta') {
    $unexpected = @($signatures | Where-Object Status -ne 'NotSigned')
    if ($unexpected.Count -ne 0) {
        throw 'unsigned-beta mode requires every GameHQ-built artifact to be consistently unsigned.'
    }
} else {
    $invalid = @($signatures | Where-Object Status -ne 'Valid')
    if ($invalid.Count -ne 0) {
        throw 'signed trust mode rejects missing or invalid Authenticode signatures.'
    }
    $subjects = @($signatures.Subject | Select-Object -Unique)
    if ($subjects.Count -ne 1 -or [string]::IsNullOrWhiteSpace($subjects[0])) {
        throw 'signed trust mode requires one consistent publisher identity.'
    }
}
if (-not (Test-Path -LiteralPath $payloadRoot -PathType Container)) {
    throw "Missing neutral program payload: $payloadRoot"
}
$payloadRoots = @(Get-ChildItem -LiteralPath $payloadRoot -Force | ForEach-Object Name)
$requiredPayloadRoots = @('GameHQ.exe', 'GameHQUpdater.exe', 'README.txt', 'LICENSE.txt',
    'THIRD_PARTY_NOTICES.md', 'app', 'licenses')
foreach ($required in $requiredPayloadRoots) {
    if ($payloadRoots -notcontains $required) { throw "Neutral payload is missing $required" }
}
foreach ($forbidden in @('portable.flag', 'Captures', 'gamehq-data', 'saveplay-data', 'playhq-data')) {
    if ($payloadRoots -contains $forbidden) { throw "Neutral payload contains forbidden path: $forbidden" }
}
foreach ($entry in $payloadRoots) {
    if ($entry -notin $requiredPayloadRoots) { throw "Neutral payload contains unexpected path: $entry" }
}
$payloadGameInput = Join-Path $payloadRoot 'app\GameInputRedist.dll'
if (-not (Test-Path -LiteralPath $payloadGameInput -PathType Leaf)) {
    throw 'Neutral payload is missing the app-local GameInput runtime.'
}
$payloadGameInputHash = (Get-FileHash -LiteralPath $payloadGameInput -Algorithm SHA256).Hash.ToLowerInvariant()
if ($payloadGameInputHash -ne $gameInputToolchain.X64RuntimeSha256) {
    throw "Neutral payload has an unpinned GameInput runtime: $payloadGameInputHash"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
function Get-ZipEntries([string]$Path) {
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try { return @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') }) }
    finally { $archive.Dispose() }
}
$portableEntries = Get-ZipEntries $portableZip
$updateEntries = Get-ZipEntries $updateZip
foreach ($required in @('GameHQ.exe', 'GameHQUpdater.exe', 'portable.flag', 'app/GameHQ.exe',
        $gameInputRuntimeRelative)) {
    if ($portableEntries -notcontains $required) { throw "Portable ZIP is missing $required" }
}
foreach ($required in @('GameHQ.exe', 'GameHQUpdater.pending.exe', 'app/GameHQ.exe',
        $gameInputRuntimeRelative, 'update-package.json')) {
    if ($updateEntries -notcontains $required) { throw "Update ZIP is missing $required" }
}
$allowedRoots = @('GameHQ.exe', 'GameHQUpdater.pending.exe', 'README.txt', 'LICENSE.txt',
    'THIRD_PARTY_NOTICES.md', 'app', 'licenses', 'update-package.json')
$forbiddenRoots = @('portable.flag', 'Captures', 'gamehq-data', 'saveplay-data', 'playhq-data', 'GameHQUpdater.exe')
foreach ($entry in $updateEntries) {
    $rootName = $entry.TrimEnd('/').Split('/')[0]
    if ($rootName -notin $allowedRoots) { throw "Update ZIP contains unexpected path: $entry" }
    if ($rootName -in $forbiddenRoots) { throw "Update ZIP contains forbidden path: $entry" }
}

$archive = [System.IO.Compression.ZipFile]::OpenRead($updateZip)
try {
    $entry = $archive.GetEntry('update-package.json')
    $reader = [System.IO.StreamReader]::new($entry.Open(), [System.Text.Encoding]::UTF8)
    try { $manifest = ($reader.ReadToEnd() | ConvertFrom-Json) } finally { $reader.Dispose() }
} finally { $archive.Dispose() }
if ($manifest.schemaVersion -ne 1 -or $manifest.productId -ne $identity.ProductId `
    -or $manifest.appVersion -ne $version -or $manifest.layoutVersion -ne 1 `
    -or $manifest.minimumUpdaterVersion -ne $minimumUpdaterVersion) {
    throw 'update-package.json is missing required or matching metadata.'
}
$expectedChecksum = "^([0-9a-f]{64}) \*$([regex]::Escape($updateName))$"
$checksumText = (Get-Content -LiteralPath $checksum -Raw).Trim()
if ($checksumText -notmatch $expectedChecksum) { throw 'Update checksum file has an invalid format or filename.' }
$actualHash = (Get-FileHash -LiteralPath $updateZip -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $Matches[1]) { throw 'Update ZIP checksum does not match.' }
$expectedSourceChecksum = "^([0-9a-f]{64}) \*$([regex]::Escape($sourceName))$"
$sourceChecksumText = (Get-Content -LiteralPath $sourceChecksum -Raw).Trim()
if ($sourceChecksumText -notmatch $expectedSourceChecksum) {
    throw 'Source checksum has an invalid format or filename.'
}
$actualSourceHash = (Get-FileHash -LiteralPath $sourceZip -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSourceHash -ne $Matches[1]) { throw 'Source ZIP checksum does not match.' }
$sourceValidationArguments = @{ ReleaseDirectory = $releaseRoot }
if ($SkipTests) { $sourceValidationArguments.SkipBuild = $true }
if (-not [string]::IsNullOrWhiteSpace($NinjaPath)) {
    $sourceValidationArguments.NinjaPath = $NinjaPath
}
& (Join-Path $PSScriptRoot 'validate-source.ps1') @sourceValidationArguments
if ($LASTEXITCODE -ne 0) { throw 'Corresponding-source validation failed.' }
foreach ($entries in @($portableEntries, $updateEntries)) {
    if ($entries -notcontains 'licenses/SOURCE_OFFER.txt') {
        throw 'Binary package is missing licenses/SOURCE_OFFER.txt.'
    }
}
$offer = Get-Content -LiteralPath (Join-Path $payloadRoot 'licenses\SOURCE_OFFER.txt') -Raw
foreach ($value in @($version, $actualSourceHash, $sourceName)) {
    if ($offer -notmatch [regex]::Escape($value)) {
        throw "SOURCE_OFFER.txt does not bind $value."
    }
}
if ($GitTag -and ($offer -notmatch [regex]::Escape($GitTag))) {
    throw 'SOURCE_OFFER.txt does not bind the requested Git tag.'
}
$packagedApp = Join-Path $root 'dist\GameHQ\app\GameHQ.exe'
$versionCheck = Start-Process -FilePath $packagedApp `
    -ArgumentList @('--assert-version', $version) -WindowStyle Hidden -Wait -PassThru
if ($versionCheck.ExitCode -ne 0) { throw 'Packaged application version does not match VERSION.' }
if ($ManifestMode -in @('test', 'production')) {
    foreach ($probe in @(
        @{ Path = $packagedApp; Arguments = @('--release-trust-self-test') },
        @{ Path = (Join-Path $payloadRoot 'GameHQUpdater.exe'); Arguments = @('--release-trust-self-test') }
    )) {
        $process = Start-Process -FilePath $probe.Path -ArgumentList $probe.Arguments `
            -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw "Release trust self-test failed: $($probe.Path)" }
    }
    if ($ManifestMode -eq 'production') {
        $trust = Get-Content (Join-Path $PSScriptRoot 'release-trust.json') -Raw | ConvertFrom-Json
        $updater = Join-Path $payloadRoot 'GameHQUpdater.exe'
        $trustReport = & $updater --release-trust-self-test
        $reportValid = $LASTEXITCODE -eq 0
        $reportValid = $reportValid -and
            ($trustReport -contains "TRUSTED KEY $($trust.currentKey.keyId)")
        $reportValid = $reportValid -and ($trustReport -contains 'TRUST TABLE production')
        $reportValid = $reportValid -and
            (@($trustReport | Where-Object { $_ -like 'TRUSTED KEY gamehq-test-*' }).Count -eq 0)
        if (-not $reportValid) {
            throw 'Production package does not contain the exact production-only trust table.'
        }
    }
}
if (-not $SkipTests) {
    $localCtest = Join-Path $root 'tools\cmake\bin\ctest.exe'
    $ctest = if (Test-Path -LiteralPath $localCtest -PathType Leaf) {
        $localCtest
    } else {
        $command = Get-Command ctest.exe -ErrorAction Stop
        $command.Source
    }
    $env:PATH = (Join-Path $root 'tools\Qt\6.8.3\mingw_64\bin') + ';' + $env:PATH
    & $ctest --test-dir (Join-Path $root 'out') -R 'tst_(releasenotes|updatedownloader|updatepreflight|updateinstaller|updatertransaction)' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Release-note or updater validation tests failed.' }
}
$toolchain = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'inno-toolchain.psd1')
$evidencePaths = @($setup, $portableZip, $updateZip, $checksum, $sourceZip, $sourceChecksum)
if ($ManifestMode -in @('test', 'production')) {
    $evidencePaths += (Join-Path $releaseRoot 'gamehq-release.json')
    $evidencePaths += (Join-Path $releaseRoot 'gamehq-release.sig')
}
$artifactEvidence = @($evidencePaths | ForEach-Object {
    [ordered]@{
        fileName = [System.IO.Path]::GetFileName($_)
        size = (Get-Item -LiteralPath $_).Length
        sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
# Which tools actually produced these bytes. Without this the evidence records
# what was built but not what built it, so a release cannot be reproduced and a
# toolchain regression cannot be traced back to the build that introduced it.
function Get-ToolVersion {
    param([string]$Command, [string[]]$Arguments)
    try {
        $output = & $Command @Arguments 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $output) { return 'unavailable' }
        return ([string[]]$output)[0].Trim()
    } catch {
        return 'unavailable'
    }
}
$ninjaForEvidence = if (-not [string]::IsNullOrWhiteSpace($NinjaPath)) {
    $NinjaPath
} elseif (-not [string]::IsNullOrWhiteSpace($env:NINJA_EXE)) {
    $env:NINJA_EXE
} else {
    'ninja'
}
$buildTools = [ordered]@{
    cmake = Get-ToolVersion 'cmake' @('--version')
    ninja = Get-ToolVersion $ninjaForEvidence @('--version')
    python = Get-ToolVersion 'python' @('--version')
    dotnetSdk = Get-ToolVersion 'dotnet' @('--version')
    qt = Get-ToolVersion 'qmake' @('-query', 'QT_VERSION')
    compiler = Get-ToolVersion 'g++' @('-dumpversion')
    powerShell = $PSVersionTable.PSVersion.ToString()
    os = [System.Environment]::OSVersion.VersionString
}
$evidence = [ordered]@{
    schemaVersion = 1
    version = $version
    minimumUpdaterVersion = $minimumUpdaterVersion
    trustMode = $TrustMode
    manifestMode = $ManifestMode
    releaseTrust = if ($ManifestMode -eq 'production') {
        $trust = Get-Content (Join-Path $PSScriptRoot 'release-trust.json') -Raw | ConvertFrom-Json
        [ordered]@{
            keyId = [string]$trust.currentKey.keyId
            publicKeySha256 = (Get-FileHash -InputStream ([IO.MemoryStream]::new(
                [Convert]::FromBase64String([string]$trust.currentKey.publicKeyBase64))) `
                -Algorithm SHA256).Hash.ToLowerInvariant()
            minimumReleaseSequence = [uint64]$trust.currentKey.minimumReleaseSequence
        }
    } else { $null }
    innoSetup = [ordered]@{ version = $toolchain.Version; installerSha256 = $toolchain.Sha256 }
    buildTools = $buildTools
    compliance = [ordered]@{
        license = 'passed'
        privacy = 'passed'
        correspondingSource = 'passed'
    }
    artifacts = $artifactEvidence
    authenticode = $signatures
} | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText((Join-Path $releaseRoot 'release-evidence.json'),
    $evidence + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "[validate] release $version passed"
