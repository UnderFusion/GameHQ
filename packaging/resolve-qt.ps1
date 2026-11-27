# Resolve the deployment kit from the selected build's CMake cache. A standalone
# source validation or CI configure step can use the checked-in release pin.
function Resolve-GameHQQtKit {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string]$BuildDirectory,
        [string]$QtPrefix,
        [switch]$RequireReleaseVersion
    )

    if ($BuildDirectory -and $QtPrefix) {
        throw 'Choose a configured build directory or an explicit Qt prefix.'
    }
    $pin = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'qt-toolchain.psd1')
    if ($BuildDirectory) {
        $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
        if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
            throw "No CMake cache for the selected Qt build: $cachePath"
        }
        $cache = Get-Content -LiteralPath $cachePath -Raw
        $qtConfig = [regex]::Match($cache, '(?m)^Qt6_DIR:[^=\r\n]+=([^\r\n]+)').Groups[1].Value
        $compiler = [regex]::Match($cache, '(?m)^CMAKE_CXX_COMPILER:[^=\r\n]+=([^\r\n]+)').Groups[1].Value
        if (-not [IO.Path]::IsPathRooted($qtConfig) -or -not [IO.Path]::IsPathRooted($compiler)) {
            throw "The build must record absolute Qt6_DIR and CMAKE_CXX_COMPILER paths: $cachePath"
        }
        $prefix = [IO.Path]::GetFullPath((Join-Path $qtConfig '..\..\..'))
        $compilerBin = Split-Path -Parent $compiler
    } else {
        if (-not $QtPrefix) {
            $QtPrefix = "tools\Qt\$($pin.Version)\$($pin.KitDirectory)"
            $RequireReleaseVersion = $true
        }
        $prefix = if ([IO.Path]::IsPathRooted($QtPrefix)) {
            [IO.Path]::GetFullPath($QtPrefix)
        } else {
            [IO.Path]::GetFullPath((Join-Path $Root $QtPrefix))
        }
        $compilerBin = Join-Path $Root "tools\Qt\Tools\$($pin.CompilerDirectory)\bin"
    }

    $prefix = $prefix.TrimEnd('\', '/')
    $bin = Join-Path $prefix 'bin'
    $qtPaths = Join-Path $bin 'qtpaths.exe'
    foreach ($required in @($qtPaths, (Join-Path $prefix 'lib\cmake\Qt6\Qt6Config.cmake'),
            (Join-Path $compilerBin 'g++.exe'))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "The selected Qt MinGW toolchain is incomplete: $required"
        }
    }
    $version = & $qtPaths --qt-version
    if ($LASTEXITCODE -ne 0 -or "$version" -notmatch '^\d+\.\d+\.\d+$') {
        throw "Could not determine the Qt version from $qtPaths"
    }
    $reportedPrefix = & $qtPaths --query QT_INSTALL_PREFIX
    if ($LASTEXITCODE -ne 0 -or -not $reportedPrefix -or
        [IO.Path]::GetFullPath($reportedPrefix).TrimEnd('\', '/') -ne $prefix.TrimEnd('\', '/')) {
        throw "The selected Qt kit reports a different installation prefix: $reportedPrefix"
    }
    if ($RequireReleaseVersion -and $version -ne $pin.Version) {
        throw "The configured release kit requires Qt $($pin.Version), found $version."
    }

    return [pscustomobject]@{
        Prefix = $prefix
        Bin = $bin
        CompilerBin = $compilerBin
        Version = $version
        ReleaseVersion = $pin.Version
    }
}
