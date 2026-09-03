[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$identity = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'distribution-identity.psd1')
$toolchain = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'inno-toolchain.psd1')
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
    throw 'VERSION must contain exactly three numeric components.'
}

$payloadRoot = Join-Path $root 'dist\.program-payload'
$releaseRoot = Join-Path $root 'dist\releases'
$compiler = Join-Path $root "tools\InnoSetup\$($toolchain.Version)\ISCC.exe"
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'bootstrap-inno.ps1')
}
$python = (Get-Command python -ErrorAction Stop).Source
$localeManifest = Join-Path $root 'i18n\locales.json'
$languageInclude = Join-Path $PSScriptRoot 'generated\InnoLanguages.iss'
$languageBootstrapInclude = Join-Path $PSScriptRoot 'generated\InnoLanguageBootstrap.iss'
$languageGenerator = Join-Path $root 'tools\i18n\generate_inno_languages.py'
$languageAudit = Join-Path $root 'tools\i18n\test_inno_languages.py'
$languageBootstrapAudit = Join-Path $root 'tools\i18n\test_inno_bootstrap.py'
$customMessageManifest = Join-Path $PSScriptRoot 'i18n\custom-messages.json'
$customMessageInclude = Join-Path $PSScriptRoot 'generated\InnoCustomMessages.iss'
$customMessageGenerator = Join-Path $root 'tools\i18n\generate_inno_custom_messages.py'
$customMessageAudit = Join-Path $root 'tools\i18n\test_inno_custom_messages.py'
& $python $languageGenerator --manifest $localeManifest --output $languageInclude `
    --bootstrap-output $languageBootstrapInclude --check
if ($LASTEXITCODE -ne 0) { throw 'Generated Inno language mapping is stale.' }
& $python $languageAudit --compiler-root (Split-Path -Parent $compiler) --require-compiler
if ($LASTEXITCODE -ne 0) { throw 'Pinned Inno language qualification failed.' }
& $python $languageBootstrapAudit
if ($LASTEXITCODE -ne 0) { throw 'Installer language bootstrap qualification failed.' }
& $python $customMessageGenerator --locales $localeManifest --messages $customMessageManifest --output $customMessageInclude --check
if ($LASTEXITCODE -ne 0) { throw 'Generated Inno CustomMessages are stale.' }
& $python $customMessageAudit
if ($LASTEXITCODE -ne 0) { throw 'Inno CustomMessages qualification failed.' }
foreach ($required in @('GameHQ.exe', 'GameHQUpdater.exe', 'app\GameHQ.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot $required) -PathType Leaf)) {
        throw "Neutral payload is missing $required; run packaging/make-dist.ps1 first."
    }
}
if (Test-Path -LiteralPath (Join-Path $payloadRoot 'portable.flag')) {
    throw 'Refusing to compile Setup from a portable payload.'
}

New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null
$setupName = $identity.ArtifactPatterns.Setup -f $version
$setupBaseName = [System.IO.Path]::GetFileNameWithoutExtension($setupName)
$versionInfo = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
$arguments = @(
    '/Qp',
    "/DAppVersion=$version",
    "/DAppVersionInfo=$versionInfo",
    "/DInstallerAppId=$($identity.InnoAppId)",
    "/DPayloadRoot=$payloadRoot",
    "/DReleaseOutput=$releaseRoot",
    "/DSetupBaseName=$setupBaseName",
    (Join-Path $PSScriptRoot 'GameHQ.iss')
)
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed ($LASTEXITCODE)." }

$setupPath = Join-Path $releaseRoot $setupName
if (-not (Test-Path -LiteralPath $setupPath -PathType Leaf)) {
    throw "Setup output was not created: $setupPath"
}
$file = Get-Item -LiteralPath $setupPath
Write-Host "[setup] ready: $setupPath ($([math]::Round($file.Length / 1MB, 1)) MB)"
