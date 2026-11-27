param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [switch]$BuildOnly,
    [switch]$RunOnly
)

$ErrorActionPreference = 'Stop'
if ($BuildOnly -and $RunOnly) { throw 'Select one execution mode.' }

$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$commonGitDir = git -C $sourceRoot rev-parse --path-format=absolute --git-common-dir
if ($LASTEXITCODE -ne 0) { throw 'Cannot locate the shared GameHQ toolchain.' }
$projectRoot = Split-Path $commonGitDir -Parent
$qt = Join-Path $projectRoot 'tools/Qt/6.8.3/mingw_64'
$mingw = Join-Path $projectRoot 'tools/Qt/Tools/mingw1310_64/bin'

$out = [IO.Path]::GetFullPath($OutputDirectory)
if (!$out.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Use a scratch output directory inside the GameHQ project.'
}
New-Item -ItemType Directory -Path $out -Force | Out-Null
$env:TEMP = $out
$env:TMP = $out
$env:PATH = "$mingw;$qt/bin;$env:PATH"
$env:QT_FORCE_STDERR_LOGGING = '1'
$src = Join-Path $sourceRoot 'src'

if (!$RunOnly) {
    # Build the exact production FIFO implementation against a scratch copy
    # of the header that exposes its state to the fixture in all compiled TUs.
    $include = Join-Path $out 'include'
    New-Item -ItemType Directory -Path "$include/capture" -Force | Out-Null
    $header = "$include/capture/AudioCapture.h"
    $original = Get-Content -Raw "$src/capture/AudioCapture.h"
    if ([regex]::Matches($original, '(?m)^private:').Count -ne 1) {
        throw 'AudioCapture header layout changed. Review fixture access.'
    }
    $original -replace '(?m)^private:', 'public:' | Set-Content -LiteralPath $header -Encoding utf8
    & "$qt/bin/moc.exe" "-I$src" $header -o "$out/moc_AudioCapture.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'AudioCapture moc failed.' }

    # Extract the live screenshot helper without linking a WGC session.
    $pipeline = Get-Content -Raw "$src/capture/FramePumpPipeline.cpp"
    $readback = [regex]::Match($pipeline, '(?ms)^QImage readBgraTexture\(.*?^\}')
    if (!$readback.Success) { throw 'Could not find the production screenshot readback helper.' }
    @('#include <QImage>', '#include <d3d11.h>', '#include <cstring>', $readback.Value) |
        Set-Content -LiteralPath "$out/readback_stage.inc" -Encoding utf8

    $common = @('-std=c++20', '-O3', '-DUNICODE', '-D_UNICODE',
                "-I$qt/include", "-I$qt/include/QtCore", "-I$src", "-L$qt/lib")
    & "$mingw/c++.exe" "-I$include" @common "$PSScriptRoot/fifo_and_timers.cpp" "$out/moc_AudioCapture.cpp" `
        "$src/capture/AudioCapture.cpp" "$src/capture/AudioSourcePolicy.cpp" `
        -lQt6Core -lole32 -lavrt -luuid -lmmdevapi -lksuser -o "$out/fifo_and_timers.exe"
    if ($LASTEXITCODE -ne 0) { throw 'FIFO and timer fixture build failed.' }

    & "$mingw/c++.exe" "-I$out" "-I$qt/include/QtGui" @common "$PSScriptRoot/gpu_cost.cpp" `
        "$src/capture/HdrCapabilities.cpp" "$src/capture/hdr/GpuToneMapper.cpp" `
        "$src/capture/hdr/HdrToneMapMath.cpp" -lQt6Gui -lQt6Core -ld3d11 -ld3dcompiler `
        -luser32 -ldxgi -lole32 -lmfplat -lmfuuid -lmf -o "$out/gpu_cost.exe"
    if ($LASTEXITCODE -ne 0) { throw 'GPU fixture build failed.' }
}

if (!$BuildOnly) {
    for ($run = 1; $run -le 3; $run++) {
        & "$out/fifo_and_timers.exe" fifo | Tee-Object -FilePath "$out/fifo-$run.txt"
        if ($LASTEXITCODE -ne 0) { throw 'FIFO fixture failed.' }
        foreach ($mode in @('timers-off', 'timers-on')) {
            & "$out/fifo_and_timers.exe" $mode | Tee-Object -FilePath "$out/$mode-$run.txt"
            if ($LASTEXITCODE -ne 0) { throw "Timer fixture failed: $mode" }
        }
        & "$out/gpu_cost.exe" | Tee-Object -FilePath "$out/gpu-$run.txt"
        if ($LASTEXITCODE -ne 0) { throw 'GPU fixture failed.' }
    }
}
