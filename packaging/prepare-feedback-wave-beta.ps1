# Assembles the local feedback-wave hardware beta (cpo-x04) from the freshly
# assembled portable output (packaging\assemble-package.ps1 -Mode Portable).
#
#   powershell -ExecutionPolicy Bypass -File packaging\prepare-feedback-wave-beta.ps1
#
# Local artifact only: it never pushes, tags or publishes. Source and output are
# restricted to the approved package folders. The authoritative identity is
# written beside the ZIP as beta-manifest.json and <package>.zip.sha256.
[CmdletBinding()]
param(
    [string]$SourcePortableDirectory = 'dist\GameHQ',
    [string]$BetaDirectory = 'dist\feedback-wave-beta'
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))

function Resolve-ProjectPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

$source = Resolve-ProjectPath $SourcePortableDirectory
$approvedSource = Resolve-ProjectPath 'dist\GameHQ'
if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($source.TrimEnd('\'), $approvedSource.TrimEnd('\'))) {
    throw "Beta source is restricted to $approvedSource"
}
$betaRoot = Resolve-ProjectPath $BetaDirectory
$approvedBetaRoot = Resolve-ProjectPath 'dist\feedback-wave-beta'
if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($betaRoot.TrimEnd('\'), $approvedBetaRoot.TrimEnd('\'))) {
    throw "Beta output is restricted to $approvedBetaRoot"
}

foreach ($required in @('GameHQ.exe', 'GameHQUpdater.exe', 'portable.flag', 'app\GameHQ.exe', 'app\GameInputRedist.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $required) -PathType Leaf)) {
        throw "Not an assembled portable package (missing $required): $source"
    }
}

$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
$commit = (& git -C $root rev-parse HEAD).Trim()
$branch = (& git -C $root rev-parse --abbrev-ref HEAD).Trim()

# The package identity promise: the compiled and packaged product inputs must
# match the recorded commit. Uncommitted packaging/docs additions are expected
# while the cpo-x04 slice is under review and do not enter the app.
$productPaths = @('CMakeLists.txt', 'VERSION', 'src', 'assets', 'i18n', 'licenses', 'third_party', 'tools/GameInput', 'packaging/README-dist.txt')
$productStatus = @(& git -C $root status --porcelain -- $productPaths)
$productClean = -not (($productStatus -join '') -match '\S')

$gameInput = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'gameinput-toolchain.psd1')

if (Test-Path -LiteralPath $betaRoot) {
    Remove-Item -LiteralPath $betaRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $betaRoot -Force | Out-Null

$betaName = "GameHQ-$version-feedback-wave-beta-portable.zip"
$betaPackage = Join-Path $betaRoot $betaName

# .NET zip creation keeps the release layout (package contents at the archive
# root, no leading folder) without the slow per-file Compress-Archive path.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $source, $betaPackage, [System.IO.Compression.CompressionLevel]::Optimal, $false)

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$packageHash = Get-Sha256 $betaPackage
$launcherHash = Get-Sha256 (Join-Path $source 'GameHQ.exe')
$appHash = Get-Sha256 (Join-Path $source 'app\GameHQ.exe')
$updaterHash = Get-Sha256 (Join-Path $source 'GameHQUpdater.exe')
$gameInputHash = Get-Sha256 (Join-Path $source 'app\GameInputRedist.dll')

foreach ($document in @(
        'docs\testing\feedback-wave-beta.md',
        'tools\manual-validation\FEEDBACK-WAVE-DUALSENSE-CHECKLIST.md',
        'tools\manual-validation\FEEDBACK-WAVE-OVERLAY-CHECKLIST.md')) {
    Copy-Item -LiteralPath (Join-Path $root $document) -Destination $betaRoot
}

$manifest = [ordered]@{
    schemaVersion      = 1
    purpose            = 'feedback-wave-hardware-beta (cpo-h01 wired DualSense, cpo-h02 borderless overlay)'
    stableRelease      = $false
    version            = $version
    branch             = $branch
    sourceCommit       = $commit
    productTreeClean   = $productClean
    packagingNote      = 'Prepared by packaging\prepare-feedback-wave-beta.ps1 from dist\GameHQ assembled by packaging\assemble-package.ps1 (out-final Release). Uncommitted packaging/docs additions do not enter the packaged app; productTreeClean covers the compiled and packaged product inputs only.'
    package            = $betaName
    sha256             = $packageHash
    launcherSha256     = $launcherHash
    appExeSha256       = $appHash
    updaterSha256      = $updaterHash
    build              = [ordered]@{
        configuration    = 'Release'
        buildDirectory   = 'out-final'
        generator        = 'Ninja'
        toolchain        = 'MinGW-w64 GCC 13.1.0 (tools\Qt\Tools\mingw1310_64)'
        qt               = '6.8.3 mingw_64 (tools\Qt\6.8.3\mingw_64)'
        gameInputRuntime = "$($gameInput.Version) (sha256 $gameInputHash)"
    }
    deviations         = @(
        'app\dxcompiler.dll and app\dxil.dll are not packaged: the local Qt 6.8.3 bin no longer ships them (windeployqt reports them missing, no packaging script references them) and the app never selects the D3D12 backend. The 0.7.7 release package carried them from an earlier environment.'
    )
    evidenceChecklists = [ordered]@{
        'cpo-h01' = 'FEEDBACK-WAVE-DUALSENSE-CHECKLIST.md'
        'cpo-h02' = 'FEEDBACK-WAVE-OVERLAY-CHECKLIST.md'
    }
    instructions       = 'feedback-wave-beta.md'
    createdAt          = (Get-Date).ToString('o')
}
[System.IO.File]::WriteAllText((Join-Path $betaRoot 'beta-manifest.json'),
    ($manifest | ConvertTo-Json -Depth 5), [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText((Join-Path $betaRoot "$betaName.sha256"),
    "$packageHash *$betaName`n", [System.Text.UTF8Encoding]::new($false))

Write-Host "[feedback-wave-beta] ready: $betaRoot"
Write-Host "  package: $betaName"
Write-Host "  sha256:  $packageHash"
Write-Host "  product tree clean: $productClean"
foreach ($file in (Get-ChildItem -LiteralPath $betaRoot -File)) {
    Write-Host ("  " + $file.Name + " (" + $file.Length + " bytes)")
}
