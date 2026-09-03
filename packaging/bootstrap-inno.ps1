[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$toolchain = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'inno-toolchain.psd1')
$installRoot = Join-Path $root "tools\InnoSetup\$($toolchain.Version)"
$compiler = Join-Path $installRoot 'ISCC.exe'
if (Test-Path -LiteralPath $compiler -PathType Leaf) {
    $compilerHash = (Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($compilerHash -ne $toolchain.CompilerSha256) {
        throw "Pinned Inno Setup compiler checksum mismatch: $compilerHash"
    }
    Write-Host "[inno] ready: $compiler"
    return
}

$downloadRoot = Join-Path $root 'tools\.downloads'
$installer = Join-Path $downloadRoot "innosetup-$($toolchain.Version).exe"
New-Item -ItemType Directory -Path $downloadRoot -Force | Out-Null
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    Invoke-WebRequest -UseBasicParsing -Uri $toolchain.Url -OutFile $installer
}

$actualHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $toolchain.Sha256) {
    throw "Inno Setup archive checksum mismatch: $actualHash"
}
$signature = Get-AuthenticodeSignature -FilePath $installer
if ($signature.Status -ne 'Valid' -or
    $signature.SignerCertificate.Subject -ne $toolchain.ExpectedSignerSubject) {
    throw "Inno Setup publisher signature is not the pinned valid signer."
}

New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
$arguments = @(
    '/PORTABLE=1', '/VERYSILENT', '/CURRENTUSER', '/SUPPRESSMSGBOXES', '/NORESTART',
    ('/DIR="' + $installRoot + '"')
)
$process = Start-Process -FilePath $installer -ArgumentList $arguments `
    -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw "Inno Setup portable bootstrap failed with exit code $($process.ExitCode)."
}
$compilerHash = (Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash.ToLowerInvariant()
if ($compilerHash -ne $toolchain.CompilerSha256) {
    throw "Pinned Inno Setup compiler checksum mismatch after install: $compilerHash"
}
Write-Host "[inno] installed $($toolchain.Version): $compiler"
