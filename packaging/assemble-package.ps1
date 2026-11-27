# Builds a clean GameHQ program payload or portable folder from CMake output.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Destination,

    [string]$BuildDirectory = 'out',

    [ValidateSet('Portable', 'Neutral')]
    [string]$Mode = 'Portable',

    [switch]$PreserveUserData
)

$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot 'resolve-qt.ps1')
. (Join-Path $PSScriptRoot 'qt-notices.ps1')
. (Join-Path $PSScriptRoot 'qt-runtime.ps1')

function Get-ProjectPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

function Remove-GeneratedPath([string]$Path) {
    # Windows can retain a just-exited process's DLL mapping briefly. Retry the
    # generated path instead of turning a normal app restart into a failed sync.
    for ($attempt = 1; $attempt -le 10; $attempt++) {
        try {
            if (Test-Path -LiteralPath $Path) {
                Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            }
            return
        } catch {
            if ($attempt -eq 10) { throw }
            Start-Sleep -Milliseconds 300
        }
    }
}

$source = Get-ProjectPath $BuildDirectory
$target = Get-ProjectPath $Destination
$devTarget = Get-ProjectPath 'build'
$distTarget = Get-ProjectPath 'dist\GameHQ'
$payloadTarget = Get-ProjectPath 'dist\.program-payload'
$allowedTargets = @($devTarget, $distTarget, $payloadTarget)

if ($allowedTargets -notcontains $target) {
    throw "Refusing to assemble outside the approved package folders: $target"
}
if ($PreserveUserData -and $target -ne $devTarget) {
    throw '-PreserveUserData is only valid for the local build package.'
}
if ($PreserveUserData -and $Mode -ne 'Portable') {
    throw '-PreserveUserData requires -Mode Portable.'
}
if ($target -eq $devTarget -and $Mode -ne 'Portable') {
    throw 'The local build package must remain portable.'
}
if ($target -eq $payloadTarget -and $Mode -ne 'Neutral') {
    throw 'The program payload must be assembled with -Mode Neutral.'
}

$qt = Resolve-GameHQQtKit -Root $root -BuildDirectory $source
$qtBin = $qt.Bin
$qtSbom = @(Get-GameHQQtSbom -Qt $qt)
$runtimeInputs = Get-GameHQQtRuntimeInputs -Qt $qt
$realExe = Join-Path $source 'GameHQ.exe'
$launcherExe = Join-Path $source 'GameHQLauncher.exe'
$updaterExe = Join-Path $source 'GameHQUpdater.exe'
$deployTool = Join-Path $qtBin 'windeployqt.exe'
$gameInputToolchain = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'gameinput-toolchain.psd1')
$gameInputRuntime = Join-Path $root "tools\GameInput\$($gameInputToolchain.Version)\GameInputRedist.dll"

& (Join-Path $PSScriptRoot 'bootstrap-gameinput.ps1')

foreach ($required in @($realExe, $launcherExe, $updaterExe, $deployTool)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing $required - configure and build $BuildDirectory first."
    }
}

Write-Host "[package] GameHQ -> $target"

if ($PreserveUserData) {
    New-Item -ItemType Directory -Path $target -Force | Out-Null

    # The resolved target was validated above. Retain only user-owned runtime
    # folders; everything else in build/ is generated and can be refreshed.
    # Keep the legacy data folder for one refresh; Paths migrates it atomically
    # to gamehq-data on the first GameHQ launch.
    $preservedNames = @('Captures', 'gamehq-data', 'saveplay-data', 'playhq-data')
    Get-ChildItem -LiteralPath $target -Force | Where-Object {
        $_.Name -notin $preservedNames
    } | ForEach-Object { Remove-GeneratedPath $_.FullName }
} else {
    if (Test-Path -LiteralPath $target) {
        Remove-GeneratedPath $target
    }
    New-Item -ItemType Directory -Path $target -Force | Out-Null
}

$app = Join-Path $target 'app'
New-Item -ItemType Directory -Path $app -Force | Out-Null

# Qt expects the real executable, its shared libraries, QML imports, and plugin
# subdirectories to live in one deployment tree.
Copy-Item -LiteralPath $realExe -Destination $app
Copy-Item -LiteralPath $gameInputRuntime -Destination $app
# Optional Telegram Integrated runtime (t22): shipped only when the pinned,
# self-built tdjson.dll exists. GameHQ loads it lazily and refuses any DLL whose
# SHA-256 differs from src/telegram/TdRuntimePin.h, so a missing file just means
# Telegram Integrated reports 'runtime not installed'.
$tdPin = Get-Content (Join-Path $root 'third_party\tdlib\tdlib-pin.json') -Raw | ConvertFrom-Json
$tdRuntime = Join-Path $root "tools\tdlib\$($tdPin.version)\tdjson.dll"
if (Test-Path -LiteralPath $tdRuntime -PathType Leaf) {
    $tdTarget = Join-Path $app 'runtime\tdlib'
    New-Item -ItemType Directory -Path $tdTarget -Force | Out-Null
    Copy-Item -LiteralPath $tdRuntime -Destination $tdTarget
    Write-Host "[package] optional TDLib runtime $($tdPin.version) included"
} else {
    Write-Host '[package] optional TDLib runtime not built; Telegram Integrated will report it as not installed'
}
$previousPath = $env:PATH
try {
    # Use the selected SDK for Qt tools and avoid Windows SDK DLL discovery.
    $env:PATH = "$qtBin;$($qt.CompilerBin)"
    & $deployTool --verbose 0 --qmldir (Join-Path $root 'src\ui\qml') `
        --no-system-d3d-compiler --no-system-dxc-compiler --no-compiler-runtime --no-ffmpeg `
        --skip-plugin-types sqldrivers (Join-Path $app 'GameHQ.exe')
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }
} finally { $env:PATH = $previousPath }

# Older windeployqt versions may still copy SQL drivers. Preserve SQLite
# explicitly and prevent drivers that need separate database client libraries.
$sqlDir = Join-Path $app 'sqldrivers'
New-Item -ItemType Directory -Path $sqlDir -Force | Out-Null
Get-ChildItem -LiteralPath $sqlDir -Filter '*.dll' -File | Where-Object Name -ne 'qsqlite.dll' |
    Remove-Item -Force
foreach ($entry in $runtimeInputs.GetEnumerator()) {
    Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $app $entry.Key) -Force
}
$null = Get-GameHQQtRuntimeIdentity -Qt $qt -AppDirectory $app
[IO.File]::WriteAllText((Join-Path $app 'qt.conf'), "[Paths]`nPrefix=.`nPlugins=.`nQmlImports=qml`n")

$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
Copy-Item -LiteralPath $launcherExe -Destination (Join-Path $target 'GameHQ.exe')
Copy-Item -LiteralPath $updaterExe -Destination (Join-Path $target 'GameHQUpdater.exe')
(Get-Content (Join-Path $PSScriptRoot 'README-dist.txt') -Raw).Replace('{VERSION}', $version) |
    Set-Content (Join-Path $target 'README.txt') -NoNewline
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $target 'LICENSE.txt')
[IO.File]::WriteAllText((Join-Path $target 'THIRD_PARTY_NOTICES.md'),
    (Get-GameHQPackageNotices -Root $root -Qt $qt), [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $root 'licenses') -Destination (Join-Path $target 'licenses') -Recurse
if ($target -eq $payloadTarget) {
    $sourceOffer = Join-Path $root 'dist\.source-offer\SOURCE_OFFER.txt'
    if (-not (Test-Path -LiteralPath $sourceOffer -PathType Leaf)) {
        throw 'Neutral release payload requires a revision-bound SOURCE_OFFER.txt.'
    }
    Copy-Item -LiteralPath $sourceOffer -Destination (Join-Path $target 'licenses\SOURCE_OFFER.txt')
}

$sbomTarget = Join-Path $target 'licenses\qt-sbom'
New-Item -ItemType Directory -Path $sbomTarget -Force | Out-Null
Copy-Item -LiteralPath $qtSbom.FullName -Destination $sbomTarget
if ($Mode -eq 'Portable') {
    New-Item -ItemType File -Path (Join-Path $target 'portable.flag') -Force | Out-Null
}

$programBytes = (Get-ChildItem -LiteralPath $app -Recurse -File |
    Measure-Object Length -Sum).Sum
$programBytes += (Get-Item -LiteralPath (Join-Path $target 'GameHQ.exe')).Length
$programBytes += (Get-Item -LiteralPath (Join-Path $target 'GameHQUpdater.exe')).Length
$programBytes += (Get-Item -LiteralPath (Join-Path $target 'README.txt')).Length
$programBytes += (Get-Item -LiteralPath (Join-Path $target 'LICENSE.txt')).Length
$sourceOfferPath = Join-Path $target 'licenses\SOURCE_OFFER.txt'
if (Test-Path -LiteralPath $sourceOfferPath -PathType Leaf) {
    $programBytes += (Get-Item -LiteralPath $sourceOfferPath).Length
}
$programBytes += (Get-Item -LiteralPath (Join-Path $target 'THIRD_PARTY_NOTICES.md')).Length
$size = [math]::Round($programBytes / 1MB, 1)
Write-Host "[package] done: $size MB"
Get-ChildItem -LiteralPath $target | ForEach-Object { Write-Host ("  " + $_.Name) }
