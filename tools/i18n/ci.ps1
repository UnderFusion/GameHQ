[CmdletBinding()]
param(
    # The lifecycle stage is chosen by the caller, never sniffed from the
    # manifest. Candidate keeps rejecting a finalized repository, which is the
    # whole point of running it on development branches.
    [ValidateSet('candidate', 'final')]
    [string]$Mode = 'candidate'
)

$ErrorActionPreference = 'Stop'
$env:PYTHONIOENCODING = 'utf-8'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$python = (Get-Command python -ErrorAction Stop).Source
$powerShell = (Get-Command powershell.exe -ErrorAction Stop).Source

function Invoke-Checked {
    param([string]$Label, [string]$Command, [string[]]$Arguments)
    Write-Host "[localization-ci] $Label"
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE."
    }
}

$before = [string]::Join("`n", @(& git -C $projectRoot status --porcelain=v1 --untracked-files=all))
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the checkout before localization verification.' }

Invoke-Checked 'canonical sixteen-locale state and generated surfaces' $powerShell @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    (Join-Path $PSScriptRoot 'locale.ps1'), 'verify'
)
if ($Mode -eq 'candidate') {
    Invoke-Checked 'candidate localization readiness, privacy, and correction evidence' $python @(
        (Join-Path $PSScriptRoot 'release_readiness.py'), '--check'
    )
} else {
    # Final evidence is written outside the checkout and then re-validated, so a
    # released repository is proven coherent and deterministic without the gate
    # ever mutating the tree or gaining permission to synthesize owner intent.
    $evidence = Join-Path ([System.IO.Path]::GetTempPath()) 'gamehq-readiness-final.json'
    Invoke-Checked 'final localization readiness, privacy, and correction evidence' $python @(
        (Join-Path $PSScriptRoot 'release_readiness.py'), '--mode', 'final', '--output', $evidence
    )
    Invoke-Checked 'final readiness evidence is deterministic' $python @(
        (Join-Path $PSScriptRoot 'release_readiness.py'), '--mode', 'final',
        '--output', $evidence, '--check'
    )
    Remove-Item -LiteralPath $evidence -Force -ErrorAction SilentlyContinue
}
Invoke-Checked 'release-note sources, bundles, publication, and integrity' $powerShell @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    (Join-Path $projectRoot 'packaging\test-release-note-assets.ps1'),
    '-PythonExecutable', $python
)

$pythonChecks = @(
    @{ Label = 'translation structure, placeholders, plurals, encoding, and provenance'; Script = 'test_verify.py' },
    @{ Label = 'installer locale mapping and pinned provenance'; Script = 'test_inno_languages.py' },
    @{ Label = 'installer CustomMessages and protected tokens'; Script = 'test_inno_custom_messages.py' },
    @{ Label = 'installer-to-application bootstrap mapping'; Script = 'test_inno_bootstrap.py' },
    @{ Label = 'auxiliary runtime and resource boundaries'; Script = 'test_auxiliary_surfaces.py' },
    @{ Label = 'CI wiring and stale-output regression'; Script = 'test_ci.py' }
    @{ Label = 'contextual linguistic-review evidence'; Script = 'test_linguistic_qa.py' }
)
foreach ($check in $pythonChecks) {
    Invoke-Checked $check.Label $python @((Join-Path $PSScriptRoot $check.Script))
}

# The readiness validator's own unit tests build their fixtures from the live
# candidate evidence snapshot, which only a pre-release checkout carries. They
# belong where code changes land - pull-request and development refs - while a
# release ref runs the validator against the actual finalized repository above.
if ($Mode -eq 'candidate') {
    Invoke-Checked 'release-readiness governance and correction fixtures' $python @(
        (Join-Path $PSScriptRoot 'test_release_readiness.py')
    )
}

$after = [string]::Join("`n", @(& git -C $projectRoot status --porcelain=v1 --untracked-files=all))
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the checkout after localization verification.' }
if ($after -cne $before) {
    throw "Localization verification modified the checkout.`nBefore:`n$before`nAfter:`n$after"
}

Write-Host "[localization-ci] all fast deterministic checks passed in $Mode mode without modifying the checkout"
