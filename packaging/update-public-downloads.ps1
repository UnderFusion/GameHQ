#Requires -Version 5.1
<#
.SYNOPSIS
    Keeps the README and website download blocks aligned with repository versions.
.DESCRIPTION
    App download buttons open GitHub's /releases/latest page, which always
    resolves to the newest non-pre-release, so promoting a release to Latest
    needs no repository change.

    The Playnite add-on link stays on an immutable package until the official
    database entry is accepted. Set $playniteAddonPublished to $true after that
    acceptance; the primary link will then become the stable Playnite add-on page.
#>
[CmdletBinding()]
param(
    [switch]$Check
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$appVersion = (Get-Content (Join-Path $repoRoot "VERSION") -Raw -Encoding UTF8).Trim()
$pluginRoot = Join-Path $repoRoot "integrations\playnite"
$pluginVersion = (Get-Content (Join-Path $pluginRoot "VERSION") -Raw -Encoding UTF8).Trim()
$installerManifest = Get-Content (Join-Path $pluginRoot "InstallerManifest.yaml") -Raw -Encoding UTF8
$playniteAddonPublished = $false
$stableAddonUrl = "https://playnite.link/addons.html#GameHQ_Integration"

foreach ($candidate in @($appVersion, $pluginVersion)) {
    if ($candidate -notmatch '^\d+\.\d+\.\d+$') {
        throw "Release version '$candidate' is not X.Y.Z"
    }
}

$packageMatch = [regex]::Match(
    $installerManifest,
    "(?ms)^\s*-\s+Version:\s*$([regex]::Escape($pluginVersion))\s*$.*?^\s+PackageUrl:\s*[""']?(?<url>https://\S+?)[""']?\s*$"
)
if (-not $packageMatch.Success) {
    throw "InstallerManifest.yaml has no package URL for Playnite $pluginVersion"
}
$pluginPackageUrl = $packageMatch.Groups["url"].Value
if ($pluginPackageUrl -match '/releases/latest(?:/|$)') {
    throw "Playnite package links must be immutable and cannot use /releases/latest"
}
$pluginPrimaryUrl = if ($playniteAddonPublished) { $stableAddonUrl } else { $pluginPackageUrl }

$latestReleaseUrl = "https://github.com/underfusion/GameHQ/releases/latest"
$setupUrl = $latestReleaseUrl
$portableUrl = $latestReleaseUrl
$publicLicenseLabel = 'GPL-3.0 licensed'
$readmeLicenseLabel = 'GPL-3.0'
$newline = [Environment]::NewLine

$readmeBlock = @(
    "<!-- public-downloads:start -->",
    '<p align="center">',
    "  <a href=""$setupUrl""><img src=""docs/assets/download-windows.svg"" width=""230"" alt=""Download GameHQ for Windows""></a>",
    "  &nbsp;",
    "  <a href=""$portableUrl""><img src=""docs/assets/download-portable.svg"" width=""230"" alt=""Download GameHQ Portable ZIP""></a>",
    "</p>",
    "<p align=""center""><sub>Windows 10+ &middot; Latest Beta &middot; $readmeLicenseLabel &middot; No telemetry</sub></p>",
    "<p align=""center""><sub>Using Playnite? <a href=""$pluginPrimaryUrl"">Get the GameHQ Integration &rarr;</a></sub></p>",
    "<!-- public-downloads:end -->"
) -join $newline

$siteBlock = @(
    "  <!-- public-downloads:start -->",
    '  <section class="download" aria-label="Downloads">',
    '    <div class="download-actions">',
    "      <a class=""download-button"" href=""$setupUrl""><span>Download for Windows</span><small>Latest GameHQ Setup</small></a>",
    "      <a class=""download-button secondary"" href=""$portableUrl""><span>Portable ZIP</span><small>Latest GameHQ &middot; no installation</small></a>",
    "    </div>",
    "    <p class=""download-meta"">Windows 10+ &middot; Latest Beta &middot; $publicLicenseLabel &middot; No telemetry</p>",
    "    <p class=""playnite-link"">Using Playnite? <a href=""$pluginPrimaryUrl"">Get the GameHQ Integration &rarr;</a></p>",
    "  </section>",
    "  <!-- public-downloads:end -->"
) -join $newline

function Sync-MarkedBlock {
    param(
        [string]$Path,
        [string]$Replacement
    )
    $content = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    $matches = [regex]::Matches(
        $content,
        '(?ms)^[ \t]*<!-- public-downloads:start -->.*?^[ \t]*<!-- public-downloads:end -->'
    )
    if ($matches.Count -ne 1) {
        throw "$Path must contain exactly one public-downloads block"
    }
    $match = $matches[0]
    if ($Check) {
        $currentBlock = $match.Value -replace "`r`n?", "`n"
        $expectedBlock = $Replacement -replace "`r`n?", "`n"
        if ($currentBlock -cne $expectedBlock) {
            throw "Public download metadata drifted in $Path"
        }
        Write-Host "[public-downloads] current: $Path"
        return
    }
    $updated = $content.Substring(0, $match.Index) +
        $Replacement +
        $content.Substring($match.Index + $match.Length)
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $updated, $utf8NoBom)
    Write-Host "[public-downloads] updated: $Path"
}

Sync-MarkedBlock -Path (Join-Path $repoRoot "README.md") -Replacement $readmeBlock
Sync-MarkedBlock -Path (Join-Path $repoRoot "site\index.html") -Replacement $siteBlock
