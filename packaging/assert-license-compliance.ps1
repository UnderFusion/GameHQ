# Validates the GPL core boundary, MIT exceptions, and release payload.
[CmdletBinding()]
param(
    [string]$PayloadDirectory = 'dist\.program-payload',
    [switch]$SkipPackage
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$failures = [Collections.Generic.List[string]]::new()

function Require-File([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { $failures.Add("missing $Label") }
}
function Require-Match([string]$Path, [string]$Pattern, [string]$Label) {
    Require-File $Path $Label
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $content = Get-Content -LiteralPath $Path -Raw
        if ($content -notmatch $Pattern) { $failures.Add("stale or missing $Label") }
    }
}

$expectedGpl = '3972DC9744F6499F0F9B2DBF76696F2AE7AD8AF9B23DDE66D6AF86C9DFB36986'
$expectedMit = '835AFB738215E42F79ABAF39E12A5A3F936A722CCA5870B753A9B1D1B19C0A7F'
$licensePaths = @(
    @{ Path = 'LICENSE'; Hash = $expectedGpl; Label = 'exact GNU GPL version 3 text' },
    @{ Path = 'licenses\MIT-legacy.txt'; Hash = $expectedMit; Label = 'legacy MIT license' },
    @{ Path = 'integrations\playnite\LICENSE'; Hash = $expectedMit; Label = 'Playnite MIT license' },
    @{ Path = 'docs\integration-protocol.LICENSE'; Hash = $expectedMit; Label = 'protocol MIT license' }
)
foreach ($entry in $licensePaths) {
    $path = Join-Path $root $entry.Path
    Require-File $path $entry.Label
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($hash -ne $entry.Hash) { $failures.Add("modified $($entry.Label)") }
    }
}

Require-Match (Join-Path $root 'docs\licensing.md') 'GPL-3\.0-only' 'GPL boundary documentation'
Require-Match (Join-Path $root 'docs\licensing.md') 'GameHQ-<version>-source\.zip' 'corresponding-source policy'
Require-Match (Join-Path $root 'docs\integration-protocol.md') 'SPDX-License-Identifier:\s*MIT' 'protocol SPDX marker'
Require-Match (Join-Path $root 'CMakeLists.txt') 'miniz/archive/refs/tags/3\.1\.2\.tar\.gz' 'miniz 3.1.2 pin'
Require-Match (Join-Path $root 'CMakeLists.txt') 'monocypher-4\.0\.3\.tar\.gz' 'Monocypher 4.0.3 pin'
Require-Match (Join-Path $root 'packaging\inno-toolchain.psd1') "Version\s*=\s*'7\.1\.0'" 'Inno Setup 7.1.0 pin'
Require-Match (Join-Path $root 'integrations\playnite\src\GameHQ.Playnite\GameHQ.Playnite.csproj') 'BouncyCastle\.Cryptography"\s+Version="2\.6\.2"' 'Bouncy Castle 2.6.2 pin'
Require-Match (Join-Path $root 'integrations\playnite\src\GameHQ.Playnite\GameHQ.Playnite.csproj') 'PlayniteSDK"\s+Version="6\.16\.0"' 'Playnite SDK 6.16.0 pin'

$stalePatterns = @(
    'GameHQ source code is available under the \[MIT License\]',
    'GameHQ first-party code is licensed under the MIT License',
    'GameHQ core uses the same MIT terms',
    'GameHQ core, Playnite integration, and public protocol[^\r\n]{0,100}MIT'
)
$claimFiles = @(& git -C $root ls-files --cached --others --exclude-standard -- `
    '*.md' '*.html' '*.qml' '*.txt' '*.ps1' '*.psd1' '*.cpp') |
    Where-Object { $_ -notmatch '^(docs/plans/|licenses/|packaging/assert-license-compliance\.ps1$)' }
foreach ($relative in $claimFiles) {
    $path = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $content = Get-Content -LiteralPath $path -Raw
    foreach ($pattern in $stalePatterns) {
        if ($content -match $pattern) { $failures.Add("stale first-party MIT claim: $relative") }
    }
}

$expectedAssets = @(
    'assets/icons/gamehq.ico', 'assets/icons/gamehq.rc', 'assets/icons/gamehq.svg',
    'assets/icons/generate_icon.py', 'assets/icons/icongen.cpp', 'assets/icons/icon-preview.html',
    'assets/installer/generate_installer_artwork.py',
    'assets/installer/wizard-large.png', 'assets/installer/wizard-small.png',
    'assets/sounds/README.md', 'assets/sounds/generate_sounds.py',
    'assets/sounds/confirm.wav', 'assets/sounds/error.wav', 'assets/sounds/favorite.wav',
    'assets/sounds/nav_tick.wav', 'assets/sounds/overlay_close.wav',
    'assets/sounds/overlay_open.wav', 'assets/sounds/replay_saved.wav',
    'assets/sounds/screenshot.wav'
) | Sort-Object
$allAssets = @(& git -C $root ls-files --cached --others --exclude-standard 'assets/**')
# The versioned release-note source, its generated bundles and its publication
# artifacts are one reviewed first-party unit, so they are asserted by shape
# instead of being enumerated file by file.
$releaseNoteAssets = @($allAssets | Where-Object { $_ -like 'assets/release-notes/*' })
$actualAssets = @($allAssets | Where-Object { $_ -notlike 'assets/release-notes/*' } | Sort-Object)
if (($expectedAssets -join "`n") -ne ($actualAssets -join "`n")) {
    $failures.Add('asset inventory changed without dependency-license review')
}
if ($releaseNoteAssets.Count -lt 1) {
    $failures.Add('versioned release-note source is missing')
}
$foreignReleaseNotes = @($releaseNoteAssets | Where-Object { $_ -notmatch '\.(json|md)$' })
if ($foreignReleaseNotes.Count -gt 0) {
    $failures.Add("non first-party release-note asset: $($foreignReleaseNotes -join ', ')")
}

if (-not $SkipPackage) {
    $payload = if ([IO.Path]::IsPathRooted($PayloadDirectory)) {
        [IO.Path]::GetFullPath($PayloadDirectory)
    } else { [IO.Path]::GetFullPath((Join-Path $root $PayloadDirectory)) }
    $approvedPayload = [IO.Path]::GetFullPath((Join-Path $root 'dist\.program-payload'))
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals($payload, $approvedPayload)) {
        throw "License package validation is restricted to $approvedPayload"
    }
    foreach ($relative in @('LICENSE.txt', 'THIRD_PARTY_NOTICES.md',
            'licenses\SOURCE_OFFER.txt', 'licenses\MIT-legacy.txt',
            'licenses\LGPL-2.1.txt', 'licenses\LGPL-3.0.txt',
            'licenses\GCC-RUNTIME-EXCEPTION.txt', 'licenses\MINGW-W64-RUNTIME.txt',
            'licenses\miniz.txt', 'licenses\monocypher.txt', 'licenses\Inno-Setup.txt',
            'licenses\qt-sbom\qtbase-6.8.3.spdx',
            'licenses\qt-sbom\qtdeclarative-6.8.3.spdx',
            'licenses\qt-sbom\qtmultimedia-6.8.3.spdx',
            'licenses\qt-sbom\qtsvg-6.8.3.spdx')) {
        Require-File (Join-Path $payload $relative) "package $relative"
    }
    $packagedLicense = Join-Path $payload 'LICENSE.txt'
    if ((Test-Path -LiteralPath $packagedLicense -PathType Leaf) -and
        (Get-FileHash -LiteralPath $packagedLicense -Algorithm SHA256).Hash -ne $expectedGpl) {
        $failures.Add('package root license is not exact GPLv3 text')
    }
    $owned = @('GameHQ.exe', 'GameHQUpdater.exe', 'app/GameHQ.exe')
    foreach ($binary in Get-ChildItem -LiteralPath $payload -Recurse -File |
            Where-Object Extension -in @('.exe', '.dll')) {
        $relative = $binary.FullName.Substring($payload.TrimEnd('\', '/').Length + 1).Replace('\', '/')
        if ($relative -in $owned) { continue }
        if ($binary.Extension -eq '.exe' -or $relative -notlike 'app/*.dll') {
            $failures.Add("unclassified release binary: $relative")
        }
    }
}

if ($failures.Count) {
    throw "License compliance failed:`n - $($failures -join "`n - ")"
}
Write-Host '[license] GPL core, MIT exceptions, dependency pins, assets, notices, and package inventory passed'
