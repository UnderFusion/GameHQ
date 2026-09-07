# Proves that packaging validation uses the explicitly selected build directory
# and never falls back to the repository default.
#
#   powershell -ExecutionPolicy Bypass -File packaging\test-build-directory-identity.ps1

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot 'resolve-build-directory.ps1')

$testRoot = Join-Path $root 'build\.build-directory-tests'
$approvedTestRoot = [System.IO.Path]::GetFullPath((Join-Path $root 'build\.build-directory-tests'))
if (Test-Path -LiteralPath $testRoot) {
    $resolvedTestRoot = (Resolve-Path -LiteralPath $testRoot).Path
    if (-not [System.StringComparer]::OrdinalIgnoreCase.Equals($resolvedTestRoot, $approvedTestRoot)) {
        throw "Unexpected build-directory test cleanup target: $resolvedTestRoot"
    }
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "[build-identity] $Message" }
}

function Assert-Throws([scriptblock]$Action, [string]$Expected, [string]$Message) {
    try {
        & $Action
    } catch {
        Assert-True ($_.Exception.Message -like "*$Expected*") `
            "$Message - expected '$Expected', got '$($_.Exception.Message)'"
        return
    }
    throw "[build-identity] $Message - no error was raised"
}

function New-FakeBinary([string]$Path, [string]$Content) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [System.IO.File]::WriteAllText($Path, $Content, [System.Text.UTF8Encoding]::new($false))
}

try {
    # A repository-shaped tree that deliberately has no 'out' directory at all,
    # so any residual dependency on the default build location fails here.
    $fakeRoot = Join-Path $testRoot 'repository'
    $selected = Join-Path $fakeRoot 'out-production'
    $payload = Join-Path $fakeRoot 'dist\.program-payload'
    $unconfigured = Join-Path $fakeRoot 'not-configured'
    foreach ($directory in @($fakeRoot, $selected, $unconfigured)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    [System.IO.File]::WriteAllText((Join-Path $selected 'CMakeCache.txt'), "CMAKE_BUILD_TYPE:STRING=Release`n")
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fakeRoot 'out'))) `
        'the fixture must not contain a default out directory'

    # 1. The selected non-default directory resolves, and the default does not
    #    exist to fall back to.
    $resolved = Resolve-BuildDirectory -Root $fakeRoot -BuildDirectory 'out-production'
    Assert-True ([System.StringComparer]::OrdinalIgnoreCase.Equals($resolved, $selected)) `
        "resolution returned $resolved instead of $selected"
    Assert-True ('out-production' -eq (Get-BuildDirectoryLabel -Root $fakeRoot -BuildDirectory $resolved)) `
        'a repository-relative label must stay relative'
    Assert-Throws { Resolve-BuildDirectory -Root $fakeRoot -BuildDirectory 'out' } `
        'does not exist' 'a missing default build directory must fail instead of being tolerated'
    Assert-Throws { Resolve-BuildDirectory -Root $fakeRoot -BuildDirectory '  ' } `
        'No build directory was selected' 'an empty selection must be rejected'
    Assert-Throws { Resolve-BuildDirectory -Root $fakeRoot -BuildDirectory 'not-configured' } `
        'not a configured CMake build tree' 'an unconfigured directory must be rejected'
    $rooted = Resolve-BuildDirectory -Root $fakeRoot -BuildDirectory $selected
    Assert-True ([System.StringComparer]::OrdinalIgnoreCase.Equals($rooted, $selected)) `
        'an absolute selection must resolve to itself'

    # 2. Native candidate tests must run in the selected directory. The stub
    #    records the arguments it received, so the assertion is about the real
    #    invocation rather than about the script text.
    $log = Join-Path $testRoot 'ctest-arguments.txt'
    $stub = Join-Path $testRoot 'stub-ctest.ps1'
    [System.IO.File]::WriteAllText($stub, @'
[System.IO.File]::WriteAllText($env:STUB_CTEST_LOG, ($args -join ' '))
exit [int]$env:STUB_CTEST_EXIT
'@)
    $env:STUB_CTEST_LOG = $log
    $env:STUB_CTEST_EXIT = '0'
    Invoke-CandidateNativeTests -Root $fakeRoot -BuildDirectory $resolved `
        -TestPattern 'tst_(releasenotes|updatertransaction)' -CTestPath $stub
    $recorded = (Get-Content -LiteralPath $log -Raw).Trim()
    Assert-True ($recorded -like "*--test-dir $selected*") `
        "ctest ran with '$recorded' instead of the selected build directory"
    Assert-True ($recorded -notlike "*$fakeRoot\out *") 'ctest must never be pointed at the default out tree'
    Assert-True ($recorded -like '*--output-on-failure*') 'failing candidate tests must print their output'

    $env:STUB_CTEST_EXIT = '3'
    Assert-Throws {
        Invoke-CandidateNativeTests -Root $fakeRoot -BuildDirectory $resolved `
            -TestPattern 'tst_releasenotes' -CTestPath $stub
    } 'Native candidate tests failed' 'a failing candidate test run must fail validation'
    $env:STUB_CTEST_EXIT = '0'

    # 3. Built and packaged binaries are compared by content, so a package that
    #    came from another build is detectable.
    New-FakeBinary (Join-Path $selected 'GameHQ.exe') 'application'
    New-FakeBinary (Join-Path $selected 'GameHQLauncher.exe') 'launcher'
    New-FakeBinary (Join-Path $selected 'GameHQUpdater.exe') 'updater'
    New-FakeBinary (Join-Path $payload 'app\GameHQ.exe') 'application'
    New-FakeBinary (Join-Path $payload 'GameHQ.exe') 'launcher'
    New-FakeBinary (Join-Path $payload 'GameHQUpdater.exe') 'updater'
    $identity = Get-CandidateBinaryIdentity -BuildDirectory $selected -PayloadRoot $payload
    Assert-True ($identity.Count -eq 3) 'every packaged GameHQ binary must be accounted for'
    Assert-True (@($identity | Where-Object { -not $_.identical }).Count -eq 0) `
        'a package copied from the selected build must match it'

    New-FakeBinary (Join-Path $payload 'app\GameHQ.exe') 'application from another build'
    $mixed = Get-CandidateBinaryIdentity -BuildDirectory $selected -PayloadRoot $payload
    $divergent = @($mixed | Where-Object { -not $_.identical })
    Assert-True ($divergent.Count -eq 1 -and $divergent[0].name -eq 'application') `
        'a package built from a different tree must be reported'

    Remove-Item -LiteralPath (Join-Path $selected 'GameHQUpdater.exe') -Force
    Assert-Throws { Get-CandidateBinaryIdentity -BuildDirectory $selected -PayloadRoot $payload } `
        'Cannot compare build and package identity' 'a missing binary must fail instead of being skipped'

    # 4. The packaging chain must keep passing the selection along, and release
    #    validation must not reintroduce a hard-coded default.
    $validate = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'validate-release.ps1') -Raw
    $makeDist = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'make-dist.ps1') -Raw
    Assert-True ($validate -match '(?m)^\s*\[string\]\$BuildDirectory') `
        'validate-release.ps1 must accept an explicit build directory'
    Assert-True ($makeDist -match 'BuildDirectory = \$BuildDirectory') `
        'make-dist.ps1 must pass its build directory into release validation'
    Assert-True ($validate -notmatch "Join-Path \`$root 'out") `
        'validate-release.ps1 must not rebuild a fixed out path'
    Assert-True ($validate -notmatch '--test-dir') `
        'validate-release.ps1 must run candidate tests through the shared resolver'
    $skipTestsIndex = $validate.IndexOf('if (-not $SkipTests)')
    $resolveIndex = $validate.IndexOf('$buildRoot = Resolve-BuildDirectory')
    Assert-True ($resolveIndex -ge 0 -and $resolveIndex -lt $skipTestsIndex) `
        '-SkipTests must never bypass build-directory resolution'

    Write-Host '[build-identity] the selected build directory flows through packaging validation'
    Write-Host '[build-identity] no code path falls back to the repository out directory'
} finally {
    Remove-Item Env:STUB_CTEST_LOG -ErrorAction SilentlyContinue
    Remove-Item Env:STUB_CTEST_EXIT -ErrorAction SilentlyContinue
}

# The stub ctest is deliberately driven to exit 3 so a failing candidate test
# run is proven to fail validation. That `exit 3` leaves $LASTEXITCODE = 3 in
# the caller scope, and PowerShell 7 propagates a lingering $LASTEXITCODE as the
# script's own exit code, so a fully passing suite would still fail CI (the
# observed exit code 1). Ending with an explicit `exit 0` on the success path
# pins the process result to zero regardless of any leaked native exit status
# or $? state, so success is reported as success.
exit 0
