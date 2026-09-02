param(
    [Parameter(Mandatory = $true)][string]$Queue,
    [Parameter(Mandatory = $true)][string]$Response,
    [string]$CanonicalResponse
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$arguments = @(
    (Join-Path $PSScriptRoot "protocol.py"),
    "--root", $root,
    "--queue", $Queue,
    "--response", $Response
)
if ($CanonicalResponse) {
    $arguments += @("--canonical-response", $CanonicalResponse)
}
& python @arguments
exit $LASTEXITCODE
