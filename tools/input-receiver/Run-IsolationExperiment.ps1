<#
.SYNOPSIS
    cpo-o06d: walks an operator through the real-game controller-isolation
    experiment with the external receiver.

.DESCRIPTION
    The automated test (tests/tst_inputreceiver.cpp) drives the shipped policy
    path with a test-owned window. This script is for the other half of the
    evidence: the same measurement while GameHQ and a REAL game are on screen.

    It starts gamehq_input_receiver.exe with a phase file, then asks the operator
    to press Enter at each phase boundary, writing the phase markers the receiver
    attributes its counters to. At the end it prints the verdict per provider and
    keeps the raw receipt.

    The receiver only observes: no injection, no hooks, no drivers, no virtual
    devices. It is a developer tool and is never shipped.

.PARAMETER Receiver
    Path to gamehq_input_receiver.exe (built by GAMEHQ_BUILD_TESTS).

.PARAMETER LogPath
    Where the receipt is written.

.PARAMETER BaselineSeconds
    How long the baseline phase runs before the first prompt.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\input-receiver\Run-IsolationExperiment.ps1 `
        -Receiver out-tests\gamehq_input_receiver.exe `
        -LogPath .claude-gui-temp\cpo-o06d-manual-receipt.log
#>
[CmdletBinding()]
param(
    [string]$Receiver = 'out-tests\gamehq_input_receiver.exe',
    [string]$LogPath = '.claude-gui-temp\cpo-o06d-manual-receipt.log',
    [int]$BaselineSeconds = 5,
    [string]$PhaseFile = ''
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Receiver)) {
    throw "Receiver not found: $Receiver (build it with: cmake --build out-tests --target gamehq_input_receiver)"
}

$logDirectory = Split-Path -Parent $LogPath
if ($logDirectory -and -not (Test-Path -LiteralPath $logDirectory)) {
    New-Item -ItemType Directory -Path $logDirectory | Out-Null
}
if (-not $PhaseFile) {
    $PhaseFile = [System.IO.Path]::ChangeExtension($LogPath, '.phases.txt')
}
Set-Content -LiteralPath $PhaseFile -Value '' -Encoding utf8

Write-Host 'GameHQ external controller receiver - isolation experiment' -ForegroundColor Cyan
Write-Host 'The receiver logs every provider from OUTSIDE GameHQ. Press a button on the pad'
Write-Host 'during EVERY phase: only pad activity can show whether input still arrives here.'
Write-Host ''

$receiverProcess = Start-Process -FilePath $Receiver -PassThru -NoNewWindow -ArgumentList @(
    '--log', (Resolve-Path -LiteralPath $logDirectory).Path + '\' + (Split-Path -Leaf $LogPath),
    '--phase-file', (Resolve-Path -LiteralPath $PhaseFile).Path,
    '--label', 'cpo-o06d manual real-game experiment',
    '--heartbeat-ms', '500'
)

# Wait for the receiver's own readiness line rather than a fixed delay: the
# providers attach at different speeds and a phase marked before then would be
# attributed to a run that had not started measuring.
$ready = $false
for ($attempt = 0; $attempt -lt 150 -and -not $ready; $attempt++) {
    Start-Sleep -Milliseconds 100
    if (Test-Path -LiteralPath $LogPath) {
        $ready = (Select-String -LiteralPath $LogPath -Pattern 'event=ready' -Quiet)
    }
}
if (-not $ready) {
    Stop-Process -Id $receiverProcess.Id -Force -ErrorAction SilentlyContinue
    throw "The receiver never reported ready; see $LogPath"
}

function Write-PhaseMarker {
    param([string]$Name)
    Add-Content -LiteralPath $PhaseFile -Value $Name -Encoding utf8
    Write-Host ("  -> phase '{0}' marked at {1:HH:mm:ss}" -f $Name, (Get-Date)) -ForegroundColor Yellow
}

try {
    Write-Host "[1/3] BASELINE - do not open the overlay yet." -ForegroundColor Green
    Write-Host "      Put the game in the foreground and press pad buttons now."
    Write-PhaseMarker 'baseline'
    Start-Sleep -Seconds $BaselineSeconds

    Write-Host ''
    Write-Host "[2/3] EXCLUSIVE - open the GameHQ overlay now (summon it with the pad)." -ForegroundColor Green
    Write-Host '      Keep pressing the same buttons while the overlay is open.'
    Write-PhaseMarker 'exclusive'
    Read-Host '      Press Enter when the overlay has been open for a few seconds'

    Write-Host ''
    Write-Host '[3/3] RESTORED - close the overlay, keep the game in front.' -ForegroundColor Green
    Write-PhaseMarker 'restored'
    Start-Sleep -Seconds $BaselineSeconds
} finally {
    if (-not $receiverProcess.HasExited) {
        # Closing the receiver's stdin is its documented stop signal.
        $receiverProcess.StandardInput.Close()
        $receiverProcess.WaitForExit(10000) | Out-Null
    }
    if (-not $receiverProcess.HasExited) {
        Stop-Process -Id $receiverProcess.Id -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ''
Write-Host 'Measured (the raw receipt is complete; these are the summary lines):' -ForegroundColor Cyan
$verdicts = Select-String -LiteralPath $LogPath -Pattern 'event=verdict'
if (-not $verdicts) {
    Write-Warning "No verdict lines in $LogPath - was the phase file written where the receiver could read it?"
} else {
    $verdicts | ForEach-Object { Write-Host ('  ' + $_.Line) }
}
Write-Host ''
Write-Host "Receipt: $LogPath"
Write-Host "Phases : $PhaseFile"
