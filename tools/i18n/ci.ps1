[CmdletBinding()]
param()

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
)
foreach ($check in $pythonChecks) {
    Invoke-Checked $check.Label $python @((Join-Path $PSScriptRoot $check.Script))
}

$after = [string]::Join("`n", @(& git -C $projectRoot status --porcelain=v1 --untracked-files=all))
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the checkout after localization verification.' }
if ($after -cne $before) {
    throw "Localization verification modified the checkout.`nBefore:`n$before`nAfter:`n$after"
}

Write-Host '[localization-ci] all fast deterministic checks passed without modifying the checkout'
