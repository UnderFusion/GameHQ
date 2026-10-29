# Narrow, reversible recovery for the released 0.8.0 healthy+journal startup loop.
# Prefer the fixed installer once published. Never use for an incomplete update.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$DiagnosticDirectory
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $PackageRoot).Path
$update = Join-Path $root '.update'
$phasePath = Join-Path $update 'transaction.phase'
$journal = Join-Path $update 'swap.manifest'
if ((Get-Content -LiteralPath $phasePath -Raw).Trim() -ne 'healthy') {
    throw 'This repair requires a completed healthy update; no files were changed.'
}
$transaction = Get-Content -LiteralPath (Join-Path $update 'transaction.json') -Raw | ConvertFrom-Json
if ($transaction.expectedVersion -ne '0.8.0' -or
    [IO.Path]::GetFullPath($transaction.packageRoot).TrimEnd('\') -ine $root.TrimEnd('\')) {
    throw 'This is not the verified 0.8.0 installation described by its transaction.'
}
function Assert-Idle {
    if (Get-Process GameHQ,GameHQUpdater -ErrorAction SilentlyContinue) {
        throw 'Close GameHQ and wait for its updater to finish before repairing.'
    }
    foreach ($name in @('Local\GameHQApplicationActive', 'Local\GameHQUpdaterActive')) {
        $mutex = $null
        if ([Threading.Mutex]::TryOpenExisting($name, [ref]$mutex)) {
            $mutex.Dispose()
            throw 'GameHQ or its updater still owns its activity mutex; no repair performed.'
        }
    }
}
Assert-Idle
$package = Join-Path $update 'downloads\GameHQ-0.8.0-win64-update.zip'
if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ine $transaction.expectedSha256) {
    throw 'The retained update package does not match the completed transaction.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($package)
try {
    foreach ($relative in @('GameHQ.exe', 'app/GameHQ.exe')) {
        $entry = $zip.Entries | Where-Object { $_.FullName.Replace('\','/') -eq $relative }
        if (-not $entry) { throw "Verified update archive is missing $relative" }
        $stream = $entry.Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $expected = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','') }
        finally { $stream.Dispose(); $sha.Dispose() }
        if ((Get-FileHash -LiteralPath (Join-Path $root $relative) -Algorithm SHA256).Hash -ine $expected) {
            throw "Installed $relative differs from the completed update; no repair performed."
        }
    }
} finally { $zip.Dispose() }
if (-not (Test-Path -LiteralPath $journal -PathType Leaf)) { throw 'There is no stale swap journal to retire.' }
$evidence = [IO.Path]::GetFullPath($DiagnosticDirectory)
if (Test-Path -LiteralPath $evidence) { throw 'Use a new diagnostic directory to preserve earlier evidence.' }
New-Item -ItemType Directory -Path $evidence | Out-Null
foreach ($name in @('transaction.phase', 'transaction.json', 'swap.manifest', 'updater.log')) {
    $file = Join-Path $update $name
    if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination $evidence }
}
Assert-Idle
if ((Get-Content -LiteralPath $phasePath -Raw).Trim() -ne 'healthy') {
    throw 'The update state changed during verification; the journal was not changed.'
}
$retiredName = 'swap.manifest.completed-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
Rename-Item -LiteralPath $journal -NewName $retiredName
[pscustomobject]@{
    Repaired = $true
    RetiredJournal = (Join-Path $update $retiredName)
    Evidence = $evidence
    Restore = 'With GameHQ closed, rename the retired journal to swap.manifest if the original state must be restored.'
} | ConvertTo-Json
