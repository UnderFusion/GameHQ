# Validates the checksum, layout, configuration, and build of a source release.
[CmdletBinding()]
param(
    [string]$ReleaseDirectory = 'dist\releases',
    [string]$NinjaPath = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot 'resolve-ninja.ps1')
$releaseRoot = if ([System.IO.Path]::IsPathRooted($ReleaseDirectory)) {
    [System.IO.Path]::GetFullPath($ReleaseDirectory)
} else { [System.IO.Path]::GetFullPath((Join-Path $root $ReleaseDirectory)) }
$approvedRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'dist\releases'))
if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals(
        $releaseRoot.TrimEnd('\', '/'), $approvedRoot.TrimEnd('\', '/'))) {
    throw "Source validation is restricted to $approvedRoot"
}

$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
$identity = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'distribution-identity.psd1')
$sourceName = $identity.ArtifactPatterns.Source -f $version
$checksumName = $identity.ArtifactPatterns.SourceChecksum -f $version
$sourcePath = Join-Path $releaseRoot $sourceName
$checksumPath = Join-Path $releaseRoot $checksumName
foreach ($required in @($sourcePath, $checksumPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing $required" }
}
$checksumText = (Get-Content -LiteralPath $checksumPath -Raw).Trim()
$pattern = "^([0-9a-f]{64}) \*$([regex]::Escape($sourceName))$"
if ($checksumText -notmatch $pattern) { throw 'Source checksum has an invalid format or filename.' }
$actualHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $Matches[1]) { throw 'Source archive checksum does not match.' }

$validationRoot = Join-Path $root 'build\.source-validation'
if (Test-Path -LiteralPath $validationRoot) { Remove-Item -LiteralPath $validationRoot -Recurse -Force }
New-Item -ItemType Directory -Path $validationRoot | Out-Null
try {
    Expand-Archive -LiteralPath $sourcePath -DestinationPath $validationRoot
    $sourceRoot = Join-Path $validationRoot "GameHQ-$version-source"
    foreach ($relative in @('CMakeLists.txt', 'VERSION', 'LICENSE', 'docs\dev-setup.md')) {
        if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot $relative) -PathType Leaf)) {
            throw "Extracted source is missing $relative"
        }
    }
    if (-not $SkipBuild) {
        $cmake = Join-Path $root 'tools\cmake\bin\cmake.exe'
        if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
            $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
        }
        $buildRoot = Join-Path $validationRoot 'out'
        $qtPrefix = Join-Path $root 'tools\Qt\6.8.3\mingw_64'
        $mingwBin = Join-Path $root 'tools\Qt\Tools\mingw1310_64\bin'
        $ninja = Resolve-GameHQNinja -NinjaPath $NinjaPath -RepositoryRoot $root
        $cCompiler = Join-Path $mingwBin 'gcc.exe'
        $cxxCompiler = Join-Path $mingwBin 'g++.exe'
        $env:PATH = "$mingwBin;$qtPrefix\bin;$env:PATH"
        & $cmake -S $sourceRoot -B $buildRoot -G Ninja `
            "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_C_COMPILER=$cCompiler" `
            "-DCMAKE_CXX_COMPILER=$cxxCompiler" "-DCMAKE_PREFIX_PATH=$qtPrefix" `
            -DCMAKE_BUILD_TYPE=Release -DGAMEHQ_ENABLE_PSEUDO_LOCALES=OFF
        if ($LASTEXITCODE -ne 0) { throw 'Source archive did not configure.' }
        & $cmake --build $buildRoot --parallel 2
        if ($LASTEXITCODE -ne 0) { throw 'Source archive did not build.' }
    }
} finally {
    $resolved = if (Test-Path -LiteralPath $validationRoot) {
        (Resolve-Path -LiteralPath $validationRoot).Path
    } else { $validationRoot }
    $approvedValidationRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build\.source-validation'))
    if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($resolved, $approvedValidationRoot)) {
        throw "Unexpected source-validation cleanup target: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}

Write-Host "[source-validate] $sourceName passed ($actualHash)"
