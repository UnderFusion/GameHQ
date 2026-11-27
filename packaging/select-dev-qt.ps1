# Select the kit used by start.bat and check that the developer cache agrees.
[CmdletBinding()]
param([string]$QtPrefix = $env:GAMEHQ_DEV_QT_PREFIX)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot 'resolve-qt.ps1')
$kit = Resolve-GameHQQtKit -Root $root -QtPrefix $QtPrefix
$cachePath = Join-Path $root 'out\CMakeCache.txt'
$reuse = $false
if ((Test-Path -LiteralPath $cachePath -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $root 'out\build.ninja') -PathType Leaf)) {
    $cache = Get-Content -LiteralPath $cachePath -Raw
    $reuse = $cache -match '(?m)^GAMEHQ_ENABLE_PSEUDO_LOCALES:BOOL=ON\r?$'
    $expected = @{
        CMAKE_HOME_DIRECTORY = $root
        Qt6_DIR = Join-Path $kit.Prefix 'lib\cmake\Qt6'
        CMAKE_C_COMPILER = Join-Path $kit.CompilerBin 'gcc.exe'
        CMAKE_CXX_COMPILER = Join-Path $kit.CompilerBin 'g++.exe'
    }
    foreach ($entry in $expected.GetEnumerator()) {
        $actual = [regex]::Match($cache, "(?m)^$($entry.Key):[^=\r\n]+=([^\r\n]+)").Groups[1].Value
        $reuse = $reuse -and ($actual.Replace('/', '\') -eq $entry.Value)
    }
}

# The batch launcher reads these three fields on one line.
"$($kit.Prefix)|$($kit.CompilerBin)|$([int](-not $reuse))"
