#Requires -Version 5.1
<#
.SYNOPSIS
    Sanity-checks the plugin scaffold: VERSION/extension.yaml agreement,
    required files present, and the identifiers reserved in the main plan
    (p1-1) haven't drifted.
#>
[CmdletBinding()]
param(
    # A GitHub pre-release package is validated without exposing it: the public
    # installer manifest must still name only older versions.
    [switch]$Prerelease
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$failures = @()

$version = (Get-Content (Join-Path $root "VERSION") -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    $failures += "VERSION '$version' is not a plain X.Y.Z"
}

$extensionYamlPath = Join-Path $root "src\GameHQ.Playnite\extension.yaml"
$extensionYaml = Get-Content $extensionYamlPath -Raw
if ($extensionYaml -notmatch "Id:\s*GameHQ_Integration") {
    $failures += "extension.yaml Id must be GameHQ_Integration"
}
if ($extensionYaml -notmatch "Version:\s*$([regex]::Escape($version))") {
    $failures += "extension.yaml Version ($extensionYaml) does not match VERSION ($version)"
}

$projectPath = Join-Path $root "src\GameHQ.Playnite\GameHQ.Playnite.csproj"
$projectXml = Get-Content $projectPath -Raw
if ($projectXml -notmatch "<Version>\s*$([regex]::Escape($version))\s*</Version>") {
    $failures += "GameHQ.Playnite.csproj Version does not match VERSION ($version)"
}
$sdkMatch = [regex]::Match(
    $projectXml,
    '<PackageReference\s+Include="PlayniteSDK"\s+Version="(?<version>\d+\.\d+\.\d+)"'
)
if (-not $sdkMatch.Success) {
    $failures += "GameHQ.Playnite.csproj must pin a plain PlayniteSDK version"
}
if ($projectXml -notmatch '<PackageReference\s+Include="BouncyCastle\.Cryptography"\s+Version="2\.6\.2"') {
    $failures += "GameHQ.Playnite.csproj must pin reviewed BouncyCastle.Cryptography 2.6.2"
}

$installerManifestPath = Join-Path $root "InstallerManifest.yaml"
$installerManifest = Get-Content $installerManifestPath -Raw
if ($installerManifest -notmatch "AddonId:\s*GameHQ_Integration") {
    $failures += "InstallerManifest.yaml AddonId must be GameHQ_Integration"
}
$manifestVersions = @([regex]::Matches($installerManifest, '(?m)^\s*-\s+Version:\s*(?<version>\d+\.\d+\.\d+)\s*$') |
    ForEach-Object { $_.Groups['version'].Value })
if ($Prerelease) {
    if ($manifestVersions -contains $version) {
        $failures += "Pre-release $version must not be listed in InstallerManifest.yaml yet"
    } elseif ($manifestVersions.Count -eq 0 -or [version]$manifestVersions[0] -ge [version]$version) {
        $failures += "InstallerManifest.yaml newest version must be older than pre-release $version"
    }
} elseif ($installerManifest -notmatch "Version:\s*$([regex]::Escape($version))") {
    $failures += "InstallerManifest.yaml Version does not match VERSION ($version)"
}
if ($sdkMatch.Success -and
    $installerManifest -notmatch "RequiredApiVersion:\s*$([regex]::Escape($sdkMatch.Groups['version'].Value))") {
    $failures += "InstallerManifest.yaml RequiredApiVersion does not match the pinned PlayniteSDK version"
}
if ($installerManifest -match "Initial scaffold|Not yet functional") {
    $failures += "InstallerManifest.yaml still contains scaffold release copy"
}
if ($installerManifest -match '/releases/latest(?:/|")') {
    $failures += "Plugin PackageUrl must never use /releases/latest"
}
$packageUrls = [regex]::Matches(
    $installerManifest,
    '(?m)^\s+PackageUrl:\s*["'']?(?<url>https://\S+?)["'']?\s*$'
)
if ($packageUrls.Count -eq 0) {
    $failures += "InstallerManifest.yaml must contain at least one immutable PackageUrl"
}
foreach ($packageUrl in $packageUrls) {
    if ($packageUrl.Groups['url'].Value -notmatch
        '/GameHQ_Playnite_Integration_\d+_\d+_\d+\.pext$') {
        $failures += "Plugin package names must identify GameHQ Playnite Integration"
    }
}
$addonManifestPath = Join-Path $root "AddonManifest.yaml"
$addonManifest = Get-Content $addonManifestPath -Raw
if ($addonManifest -notmatch "AddonId:\s*GameHQ_Integration") {
    $failures += "AddonManifest.yaml AddonId must be GameHQ_Integration"
}
if ($addonManifest -notmatch "Type:\s*Generic") {
    $failures += "AddonManifest.yaml Type must be Generic"
}
$stableInstallerUrl =
    "https://raw.githubusercontent.com/underfusion/GameHQ/main/integrations/playnite/InstallerManifest.yaml"
if ($addonManifest -notmatch [regex]::Escape($stableInstallerUrl)) {
    $failures += "AddonManifest.yaml must use the stable main InstallerManifest URL"
}
if ((Get-FileHash -LiteralPath (Join-Path $root 'LICENSE') -Algorithm SHA256).Hash -ne
    '835AFB738215E42F79ABAF39E12A5A3F936A722CCA5870B753A9B1D1B19C0A7F') {
    $failures += 'Playnite LICENSE must remain the reviewed MIT text'
}

$requiredPaths = @(
    "src\GameHQ.Playnite\GameHQ.Playnite.csproj",
    "src\GameHQ.Playnite\GameHQPlugin.cs",
    "AddonManifest.yaml",
    "InstallerManifest.yaml",
    "RELEASE_CHECKLIST.md",
    "packaging\prepare-release.ps1",
    "CHANGELOG.md",
    "README.md",
    "LICENSE",
    "LICENSES\BouncyCastle.Cryptography.txt"
)
foreach ($relative in $requiredPaths) {
    if (-not (Test-Path (Join-Path $root $relative))) {
        $failures += "Missing required file: $relative"
    }
}

if ($failures.Count -gt 0) {
    Write-Host "[playnite-verify] FAILED:"
    foreach ($f in $failures) { Write-Host "  - $f" }
    exit 1
}

$mode = if ($Prerelease) { ', pre-release, public manifest offers ' + $manifestVersions[0] } else { '' }
Write-Host "[playnite-verify] OK (version $version$mode)"
