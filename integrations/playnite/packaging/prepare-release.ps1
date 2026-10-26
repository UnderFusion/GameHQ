#Requires -Version 5.1
<#
.SYNOPSIS
    Builds and validates a Playnite release, then emits reproducible evidence.
.DESCRIPTION
    Publication is opt-in twice. Dedicated plugin releases always use
    playnite-vX.Y.Z and gh's explicit --latest=false guard.
#>
[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [switch]$LegacyAttachedToAppRelease,
    # Package a GitHub pre-release: the public InstallerManifest.yaml and
    # AddonManifest.yaml stay exactly as on main, so Playnite never offers it.
    [switch]$Prerelease,
    [switch]$Publish,
    [switch]$ApprovePublication
)

$ErrorActionPreference = "Stop"
$pluginRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$version = (Get-Content (Join-Path $pluginRoot "VERSION") -Raw -Encoding UTF8).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION '$version' is not X.Y.Z"
}
$tag = "playnite-v$version"
$assetName = "GameHQ_Playnite_Integration_$($version -replace '\.', '_').pext"
$assetPath = Join-Path $pluginRoot "dist\$assetName"
$installerPath = Join-Path $pluginRoot "InstallerManifest.yaml"
$addonPath = Join-Path $pluginRoot "AddonManifest.yaml"
$manifest = Get-Content $installerPath -Raw -Encoding UTF8
$versionEntries = @([regex]::Matches($manifest, '(?m)^\s*-\s+Version:\s*(?<version>\d+\.\d+\.\d+)\s*$'))
$expectedFutureUrl =
    "https://github.com/underfusion/GameHQ/releases/download/$tag/$assetName"
if ($Prerelease) {
    if ($LegacyAttachedToAppRelease -or $Publish) {
        throw "Pre-release packages are published by hand with --prerelease --latest=false"
    }
    foreach ($public in @("InstallerManifest.yaml", "AddonManifest.yaml")) {
        git -C $repoRoot diff --quiet origin/main -- "integrations/playnite/$public"
        if ($LASTEXITCODE -ne 0) { throw "$public must stay identical to origin/main for a pre-release" }
    }
} elseif ($versionEntries.Count -eq 0 -or $versionEntries[0].Groups["version"].Value -ne $version) {
    throw "The first Packages entry must match VERSION $version"
}
if (($versionEntries | ForEach-Object { $_.Groups["version"].Value } | Select-Object -Unique).Count -ne
    $versionEntries.Count) {
    throw "InstallerManifest.yaml contains duplicate package versions"
}

if ($Prerelease) {
    $packageUrl = $expectedFutureUrl
} else {
$urlMatch = [regex]::Match(
    $manifest,
    "(?ms)^\s*-\s+Version:\s*$([regex]::Escape($version))\s*$.*?^\s+PackageUrl:\s*[""']?(?<url>https://\S+?)[""']?\s*$"
)
if (-not $urlMatch.Success) {
    throw "No PackageUrl exists for $version"
}
$packageUrl = $urlMatch.Groups["url"].Value
}
if ($packageUrl -match '/releases/latest(?:/|$)') {
    throw "Plugin PackageUrl cannot use /releases/latest"
}
if (-not $LegacyAttachedToAppRelease -and $packageUrl -cne $expectedFutureUrl) {
    throw "PackageUrl must be the immutable $tag release asset: $expectedFutureUrl"
}
if ($LegacyAttachedToAppRelease -and $Publish) {
    throw "Legacy app-attached packages cannot be published by this script"
}
if ($Publish -and -not $ApprovePublication) {
    throw "Publication requires both -Publish and -ApprovePublication"
}

& (Join-Path $PSScriptRoot "verify.ps1") -Prerelease:$Prerelease
if ($LASTEXITCODE -ne 0) { throw "verify.ps1 failed with exit code $LASTEXITCODE" }

$testProject = Join-Path $pluginRoot "tests\GameHQ.Playnite.Tests\GameHQ.Playnite.Tests.csproj"
dotnet restore $testProject --locked-mode
if ($LASTEXITCODE -ne 0) { throw "dotnet restore failed with exit code $LASTEXITCODE" }
dotnet test $testProject --no-restore -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "dotnet test failed with exit code $LASTEXITCODE" }
& (Join-Path $PSScriptRoot "package.ps1") -Configuration $Configuration
if (-not (Test-Path -LiteralPath $assetPath)) {
    throw "Expected package was not produced: $assetPath"
}
$packageHash = (Get-FileHash -LiteralPath $assetPath -Algorithm SHA256).Hash.ToLowerInvariant()
$remotePackageHash = $null
if ($LegacyAttachedToAppRelease) {
    $remoteCopy = Join-Path ([System.IO.Path]::GetTempPath()) (
        "gamehq-playnite-$([Guid]::NewGuid().ToString('N')).pext"
    )
    try {
        Invoke-WebRequest -Uri $packageUrl -OutFile $remoteCopy -UseBasicParsing
        $remotePackageHash =
            (Get-FileHash -LiteralPath $remoteCopy -Algorithm SHA256).Hash.ToLowerInvariant()
    } finally {
        if (Test-Path -LiteralPath $remoteCopy) {
            Remove-Item -LiteralPath $remoteCopy -Force
        }
    }
}

# The pre-release leaves both public manifests byte-identical to main, where
# Toolbox already validated them; there is nothing new for Toolbox to check.
$installerOutput = "skipped: pre-release, public manifests identical to origin/main"
$addonOutput = $installerOutput
if (-not $Prerelease) {
$toolboxCandidates = @(
    (Join-Path $env:LOCALAPPDATA "Playnite\Toolbox.exe"),
    (Join-Path $env:ProgramFiles "Playnite\Toolbox.exe")
)
$toolbox = $toolboxCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $toolbox) {
    throw "Playnite Toolbox.exe was not found"
}
$installerOutput = (& $toolbox verify installer $installerPath 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw "Toolbox installer verification failed: $installerOutput" }
$addonOutput = (& $toolbox verify addon $addonPath 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0) { throw "Toolbox add-on verification failed: $addonOutput" }
}

$distDir = Join-Path $pluginRoot "dist"
$notesPath = Join-Path $distDir "release-notes.md"
$evidencePath = Join-Path $distDir "release-evidence.json"
$notes = @(
    "# GameHQ Playnite Integration $version",
    "",
    "See integrations/playnite/CHANGELOG.md for release details.",
    "",
    "This plugin release is independent of the GameHQ application release."
) -join [Environment]::NewLine
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($notesPath, $notes, $utf8NoBom)
$commit = (git -C $repoRoot rev-parse HEAD).Trim()
$evidence = [ordered]@{
    version = $version
    tag = $tag
    asset = $assetName
    packageUrl = $packageUrl
    sha256 = $packageHash
    commit = $commit
    makeLatest = $false
    prerelease = [bool]$Prerelease
    remoteSha256 = $remotePackageHash
    installerToolbox = $installerOutput
    addonToolbox = $addonOutput
    preparedAtUtc = [DateTime]::UtcNow.ToString("o")
    published = $false
}

if ($Publish) {
    $dirty = git -C $repoRoot status --porcelain
    if ($dirty) { throw "Publication requires a clean worktree" }
    $latestBefore = (gh api repos/underfusion/GameHQ/releases/latest --jq .tag_name).Trim()
    if ($latestBefore -notmatch '^v\d+\.\d+\.\d+$') {
        throw "Latest must remain a GameHQ application release, found '$latestBefore'"
    }
    gh release create $tag $assetPath --repo underfusion/GameHQ `
        --title "GameHQ Playnite Integration $version" `
        --notes-file $notesPath --latest=false --target $commit
    if ($LASTEXITCODE -ne 0) { throw "gh release create failed with exit code $LASTEXITCODE" }
    $latestAfter = (gh api repos/underfusion/GameHQ/releases/latest --jq .tag_name).Trim()
    if ($latestAfter -cne $latestBefore) {
        throw "Safety gate failed: Latest changed from $latestBefore to $latestAfter"
    }
    $remoteCopy = Join-Path ([System.IO.Path]::GetTempPath()) (
        "gamehq-playnite-$([Guid]::NewGuid().ToString('N')).pext"
    )
    try {
        Invoke-WebRequest -Uri $expectedFutureUrl -OutFile $remoteCopy -UseBasicParsing
        $remotePackageHash =
            (Get-FileHash -LiteralPath $remoteCopy -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($remotePackageHash -cne $packageHash) {
            throw "Published package hash differs from the prepared asset"
        }
    } finally {
        if (Test-Path -LiteralPath $remoteCopy) {
            Remove-Item -LiteralPath $remoteCopy -Force
        }
    }
    $evidence.remoteSha256 = $remotePackageHash
    $evidence.published = $true
}

[System.IO.File]::WriteAllText(
    $evidencePath,
    ($evidence | ConvertTo-Json -Depth 4),
    $utf8NoBom
)
Write-Host "[playnite-release] prepared $assetName"
Write-Host "[playnite-release] SHA-256 $packageHash"
Write-Host "[playnite-release] make_latest=false"
Write-Host "[playnite-release] evidence $evidencePath"
if (-not $Publish) {
    Write-Host "[playnite-release] not published (explicit approval is required)"
}
