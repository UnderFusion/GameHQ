# Shared build-directory identity for packaging and release validation.
#
# The build that is packaged must be the build that is tested. Dot-source this
# file; it defines functions only, performs no work when loaded, and leaves the
# caller's strict-mode and error-action settings alone.

function Resolve-BuildDirectory {
    <#
        .SYNOPSIS
        Resolve an explicitly selected CMake build directory, or fail closed.

        .DESCRIPTION
        Never falls back to the repository default. A missing, empty or
        unconfigured selection is an error with the selected path in the text,
        so a release operator sees which directory was actually chosen.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$BuildDirectory
    )
    if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
        throw 'No build directory was selected. Pass -BuildDirectory explicitly (for example out or out-production).'
    }
    $resolved = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
        [System.IO.Path]::GetFullPath($BuildDirectory)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $Root $BuildDirectory))
    }
    $resolved = $resolved.TrimEnd('\', '/')
    if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
        throw "Selected build directory does not exist: $resolved. Configure and build it before packaging."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $resolved 'CMakeCache.txt') -PathType Leaf)) {
        throw "Selected build directory is not a configured CMake build tree (no CMakeCache.txt): $resolved"
    }
    return $resolved
}

function Get-BuildDirectoryLabel {
    <#
        .SYNOPSIS
        Describe a resolved build directory relative to the repository.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$BuildDirectory
    )
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    if ($BuildDirectory.StartsWith("$rootFull\", [System.StringComparison]::OrdinalIgnoreCase)) {
        return $BuildDirectory.Substring($rootFull.Length + 1).Replace('\', '/')
    }
    return $BuildDirectory.Replace('\', '/')
}

function Get-CandidateBinaryIdentity {
    <#
        .SYNOPSIS
        Pair every built binary with the packaged copy derived from it.

        .DESCRIPTION
        assemble-package.ps1 copies these three executables out of the selected
        build directory, so their hashes answer the only question release
        evidence cannot answer otherwise: is the tested binary the packaged
        binary?
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$BuildDirectory,
        [Parameter(Mandatory = $true)][string]$PayloadRoot
    )
    $pairs = @(
        @{ Name = 'application'; Built = 'GameHQ.exe'; Packaged = 'app\GameHQ.exe' },
        @{ Name = 'launcher'; Built = 'GameHQLauncher.exe'; Packaged = 'GameHQ.exe' },
        @{ Name = 'updater'; Built = 'GameHQUpdater.exe'; Packaged = 'GameHQUpdater.exe' }
    )
    return @($pairs | ForEach-Object {
        $built = Join-Path $BuildDirectory $_.Built
        $packaged = Join-Path $PayloadRoot $_.Packaged
        foreach ($required in @($built, $packaged)) {
            if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
                throw "Cannot compare build and package identity, missing: $required"
            }
        }
        $builtHash = (Get-FileHash -LiteralPath $built -Algorithm SHA256).Hash.ToLowerInvariant()
        $packagedHash = (Get-FileHash -LiteralPath $packaged -Algorithm SHA256).Hash.ToLowerInvariant()
        [ordered]@{
            name = $_.Name
            builtFile = $_.Built
            packagedFile = $_.Packaged.Replace('\', '/')
            builtSha256 = $builtHash
            packagedSha256 = $packagedHash
            identical = ($builtHash -eq $packagedHash)
        }
    })
}

function Get-SourceCommit {
    <#
        .SYNOPSIS
        Record the source commit the candidate was built from, or 'unavailable'.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Root)
    try {
        $commit = & git -C $Root rev-parse HEAD 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $commit) { return 'unavailable' }
        return ([string[]]$commit)[0].Trim()
    } catch {
        return 'unavailable'
    }
}

function Invoke-CandidateNativeTests {
    <#
        .SYNOPSIS
        Run the packaged candidate's native tests in the selected build tree.

        .DESCRIPTION
        The test directory is always the caller's resolved build directory. No
        code path here reads the repository default, so validation cannot
        describe a different build than the one that was packaged.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$BuildDirectory,
        [Parameter(Mandatory = $true)][string]$TestPattern,
        [string]$CTestPath = ''
    )
    $ctest = if (-not [string]::IsNullOrWhiteSpace($CTestPath)) {
        $CTestPath
    } else {
        $localCTest = Join-Path $Root 'tools\cmake\bin\ctest.exe'
        if (Test-Path -LiteralPath $localCTest -PathType Leaf) {
            $localCTest
        } else {
            (Get-Command ctest.exe -ErrorAction Stop).Source
        }
    }
    & $ctest --test-dir $BuildDirectory -R $TestPattern --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "Native candidate tests failed in the selected build directory: $BuildDirectory"
    }
}
