[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PayloadRoot,
    [Parameter(Mandatory = $true)]
    [string]$PortableZip,
    [Parameter(Mandatory = $true)]
    [string]$UpdateZip
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$workRoot = Join-Path $root 'out\package-localization'
$reportPath = Join-Path $workRoot 'report.json'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-ZipEntryEvidence([string]$ArchivePath, [string]$EntryName) {
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $entry = @($archive.Entries | Where-Object {
            $_.FullName.Replace('\', '/') -ceq $EntryName
        })[0]
        if ($null -eq $entry) { throw "$ArchivePath is missing $EntryName." }
        $stream = $entry.Open()
        try {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            try {
                $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
            }
            finally { $sha.Dispose() }
        } finally { $stream.Dispose() }
        return [ordered]@{ path = $EntryName; size = $entry.Length; sha256 = $hash }
    } finally { $archive.Dispose() }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$payload = [System.IO.Path]::GetFullPath($PayloadRoot)
$portable = [System.IO.Path]::GetFullPath($PortableZip)
$update = [System.IO.Path]::GetFullPath($UpdateZip)
foreach ($path in @($payload, $portable, $update)) {
    Assert-True $path.StartsWith(($root.TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase) `
        "Package localization input escaped the repository: $path"
}
foreach ($path in @($portable, $update)) {
    Assert-True (Test-Path -LiteralPath $path -PathType Leaf) "Missing package archive: $path"
}
$application = Join-Path $payload 'app\GameHQ.exe'
Assert-True (Test-Path -LiteralPath $application -PathType Leaf) `
    "Neutral payload is missing app\GameHQ.exe."

if (Test-Path -LiteralPath $workRoot) {
    Remove-Item -LiteralPath $workRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $workRoot -Force | Out-Null
$embeddedReportPath = Join-Path $workRoot 'embedded-assets.json'
$probe = Start-Process -FilePath $application -ArgumentList @(
    '--localization-assets-self-test', ('"' + $embeddedReportPath + '"')
) -WindowStyle Hidden -Wait -PassThru
Assert-True ($probe.ExitCode -eq 0) "Packaged localization probe failed with exit code $($probe.ExitCode)."
Assert-True (Test-Path -LiteralPath $embeddedReportPath -PathType Leaf) `
    'Packaged localization probe did not produce evidence.'
$embedded = Get-Content -LiteralPath $embeddedReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-True ($embedded.production_locale_count -eq 16) 'Packaged app does not expose exactly sixteen production locales.'
Assert-True ($embedded.catalog_count -eq 16) 'Packaged app does not contain sixteen valid Qt catalogs.'
Assert-True ($embedded.release_note_bundle_count -eq 16) 'Packaged app does not contain sixteen verified release-note bundles.'
Assert-True (-not [bool]$embedded.update_authorization_input) `
    'Localized presentation metadata must not be an update authorization input.'
$representatives = @($embedded.locales | Where-Object representative_smoke | ForEach-Object locale)
Assert-True (($representatives -join '|') -ceq 'en-US|pl-PL|zh-Hant|th-TH') `
    "Representative packaged locale probes differ: $($representatives -join ', ')."
$fallbackProbe = @($embedded.locales | Where-Object locale -eq 'pl-PL')[0]
Assert-True ($fallbackProbe.fallback_documents -gt 0) `
    'The packaged Polish probe did not exercise the approved whole-document English history fallback.'

$payloadApp = Get-Item -LiteralPath $application
$payloadHash = (Get-FileHash -LiteralPath $application -Algorithm SHA256).Hash.ToLowerInvariant()
$portableApp = Get-ZipEntryEvidence $portable 'app/GameHQ.exe'
$updateApp = Get-ZipEntryEvidence $update 'app/GameHQ.exe'
foreach ($entry in @($portableApp, $updateApp)) {
    Assert-True ($entry.size -eq $payloadApp.Length -and $entry.sha256 -ceq $payloadHash) `
        "$($entry.path) does not match the localization-verified payload application bytes."
}

foreach ($archivePath in @($portable, $update)) {
    $archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        foreach ($entry in $archive.Entries) {
            $normalized = $entry.FullName.Replace('\', '/')
            Assert-True (-not ($normalized -match '^(?:i18n|assets/release-notes)/')) `
                "$archivePath contains loose localization data that can skew from embedded resources: $normalized"
        }
    } finally { $archive.Dispose() }
}

$report = [ordered]@{
    schema_version = 1
    production_locale_count = 16
    catalog_count = 16
    release_note_bundle_count = 16
    representative_locales = $representatives
    whole_document_fallback_locale = 'pl-PL'
    update_authorization_input = $false
    payload_application = [ordered]@{
        path = 'app/GameHQ.exe'
        size = $payloadApp.Length
        sha256 = $payloadHash
    }
    portable_application = $portableApp
    update_application = $updateApp
    embedded_evidence_sha256 = (Get-FileHash -LiteralPath $embeddedReportPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Host '[package-localization] 16 catalogs and 16 release-note bundles verified in payload and archives'
Write-Host '[package-localization] representative smoke: en-US, pl-PL, zh-Hant, th-TH'
Write-Host "[package-localization] report: $reportPath"
