# Assembles and validates the portable and update-only release artifacts.
#
#   powershell -ExecutionPolicy Bypass -File packaging\make-dist.ps1

[CmdletBinding()]
param(
    [switch]$SkipTests,
    [string]$BuildDirectory = 'out',
    [ValidateSet('unsigned-beta', 'signed')]
    [string]$TrustMode = $env:RELEASE_TRUST_MODE,
    [ValidateSet('none', 'test', 'production')]
    [string]$ManifestMode = 'none',
    [string]$NinjaPath = '',
    [string]$GitTag = ''
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
$minimumUpdaterVersion = (Get-Content (Join-Path $PSScriptRoot 'minimum-updater-version.txt') -Raw).Trim()
$identity = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'distribution-identity.psd1')
if ($minimumUpdaterVersion -notmatch '^\d+\.\d+\.\d+$' `
    -or [version]$minimumUpdaterVersion -gt [version]$version) {
    throw 'minimum-updater-version.txt must contain a valid version no newer than VERSION.'
}
if ([string]::IsNullOrWhiteSpace($TrustMode)) {
    throw 'Set RELEASE_TRUST_MODE=unsigned-beta (or pass -TrustMode) explicitly.'
}
$dist = Join-Path $root 'dist'
$portableRoot = Join-Path $dist 'GameHQ'
$payloadRoot = Join-Path $dist '.program-payload'
$releaseRoot = Join-Path $dist 'releases'
$updateRoot = Join-Path $dist '.update-package-staging'
$sourceOfferRoot = Join-Path $dist '.source-offer'

foreach ($path in @($portableRoot, $releaseRoot, $updateRoot, $sourceOfferRoot)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    New-Item -ItemType Directory -Path $path -Force | Out-Null
}

& (Join-Path $PSScriptRoot 'prepare-source.ps1') `
    -ReleaseDirectory $releaseRoot -Revision 'HEAD' -GitTag $GitTag

& (Join-Path $PSScriptRoot 'assemble-package.ps1') `
    -BuildDirectory $BuildDirectory `
    -Destination 'dist\.program-payload' `
    -Mode Neutral

# Portable is the neutral payload plus exactly one mode marker. Setup consumes
# the payload unchanged, and update staging derives its strict allowlist below.
Copy-Item -Path (Join-Path $payloadRoot '*') -Destination $portableRoot -Recurse
New-Item -ItemType File -Path (Join-Path $portableRoot 'portable.flag') -Force | Out-Null

$portableZip = Join-Path $releaseRoot ($identity.ArtifactPatterns.Portable -f $version)
$updateZip = Join-Path $releaseRoot ($identity.ArtifactPatterns.Update -f $version)
Compress-Archive -Path (Join-Path $portableRoot '*') -DestinationPath $portableZip -CompressionLevel Optimal

foreach ($name in @('GameHQ.exe', 'README.txt', 'LICENSE.txt',
        'THIRD_PARTY_NOTICES.md', 'app', 'licenses')) {
    Copy-Item -LiteralPath (Join-Path $payloadRoot $name) -Destination $updateRoot -Recurse
}
Copy-Item -LiteralPath (Join-Path $payloadRoot 'GameHQUpdater.exe') `
    -Destination (Join-Path $updateRoot 'GameHQUpdater.pending.exe')
$manifest = [ordered]@{
    schemaVersion = 1
    productId = $identity.ProductId
    appVersion = $version
    layoutVersion = 1
    minimumUpdaterVersion = $minimumUpdaterVersion
} | ConvertTo-Json -Compress
[System.IO.File]::WriteAllText((Join-Path $updateRoot 'update-package.json'), $manifest,
    [System.Text.UTF8Encoding]::new($false))
Compress-Archive -Path (Join-Path $updateRoot '*') -DestinationPath $updateZip -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $updateZip -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$updateZip.sha256", "$hash *$([System.IO.Path]::GetFileName($updateZip))`n",
    [System.Text.UTF8Encoding]::new($false))
Remove-Item -LiteralPath $updateRoot -Recurse -Force

& (Join-Path $PSScriptRoot 'build-setup.ps1')
if ($LASTEXITCODE -ne 0) { throw "Setup build failed ($LASTEXITCODE)" }

if ($ManifestMode -in @('test', 'production')) {
    $sequence = (Get-Content (Join-Path $PSScriptRoot 'release-sequence.txt') -Raw).Trim()
    if ($sequence -notmatch '^[1-9][0-9]*$') { throw 'release-sequence.txt must contain a positive integer.' }
    $publishedAt = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ', [Globalization.CultureInfo]::InvariantCulture)
    $tool = Join-Path $root 'tools\release-manifest\GameHQ.ReleaseManifest.Tool\GameHQ.ReleaseManifest.Tool.csproj'
    & dotnet restore $tool --locked-mode
    if ($LASTEXITCODE -ne 0) { throw "Release manifest tool restore failed ($LASTEXITCODE)" }
    $manifestArguments = @(
        if ($ManifestMode -eq 'test') { 'generate-test' } else { 'generate-production' }
        '--release-dir', $releaseRoot,
        '--version', $version,
        '--minimum-updater-version', $minimumUpdaterVersion,
        '--sequence', $sequence,
        '--published-at-utc', $publishedAt
    )
    if ($ManifestMode -eq 'production') {
        $trust = Get-Content (Join-Path $PSScriptRoot 'release-trust.json') -Raw | ConvertFrom-Json
        $manifestArguments += @('--key-id', [string]$trust.currentKey.keyId)
        if ([string]::IsNullOrWhiteSpace($env:GAMEHQ_RELEASE_ED25519_SEED_BASE64)) {
            $manifestArguments += @('--credential-target', [string]$trust.currentKey.credentialTarget)
        }
    }
    & dotnet run --project $tool --configuration Release --no-restore -- @manifestArguments
    if ($LASTEXITCODE -ne 0) { throw "$ManifestMode manifest generation failed ($LASTEXITCODE)" }
}

$validationArguments = @{
    ReleaseDirectory = $releaseRoot
    TrustMode = $TrustMode
    ManifestMode = $ManifestMode
    GitTag = $GitTag
    # The build that was packaged above is the build that must be validated.
    BuildDirectory = $BuildDirectory
}
if ($SkipTests) { $validationArguments.SkipTests = $true }
if (-not [string]::IsNullOrWhiteSpace($NinjaPath)) {
    $validationArguments.NinjaPath = $NinjaPath
}
& (Join-Path $PSScriptRoot 'validate-release.ps1') @validationArguments
if ($LASTEXITCODE -ne 0) { throw "Release validation failed ($LASTEXITCODE)" }

Write-Host "[release] ready: $releaseRoot"
