# Runtime files whose origin or presence must not depend on windeployqt's host
# discovery. Qt Quick uses D3D11 by default on Windows, as does capture.
function Get-GameHQQtRuntimeInputs {
    param($Qt)

    $files = [ordered]@{
        'd3dcompiler_47.dll' = Join-Path $Qt.Bin 'd3dcompiler_47.dll'
        'sqldrivers\qsqlite.dll' = Join-Path $Qt.Prefix 'plugins\sqldrivers\qsqlite.dll'
    }
    foreach ($name in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
        $files[$name] = Join-Path $Qt.CompilerBin $name
    }
    foreach ($component in @('avcodec', 'avformat', 'avutil', 'swresample', 'swscale')) {
        $matchesInKit = @(Get-ChildItem -LiteralPath $Qt.Bin -Filter "$component-*.dll" -File |
            Where-Object Name -match "^$component-\d+\.dll$")
        if ($matchesInKit.Count -ne 1) {
            throw "Expected one $component runtime DLL in the selected Qt kit: $($Qt.Bin)"
        }
        $files[$matchesInKit[0].Name] = $matchesInKit[0].FullName
    }
    foreach ($path in $files.Values) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Missing required runtime input: $path"
        }
    }
    return $files
}

function Get-GameHQQtRuntimeIdentity {
    param($Qt, [string]$AppDirectory)

    $files = Get-GameHQQtRuntimeInputs -Qt $Qt
    foreach ($name in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Network.dll', 'Qt6Multimedia.dll',
            'Qt6Quick.dll', 'Qt6Sql.dll')) {
        $files[$name] = Join-Path $Qt.Bin $name
    }
    foreach ($plugin in @('platforms\qwindows.dll', 'multimedia\ffmpegmediaplugin.dll')) {
        $files[$plugin] = Join-Path $Qt.Prefix "plugins\$plugin"
    }

    $sqlDrivers = @(Get-ChildItem -LiteralPath (Join-Path $AppDirectory 'sqldrivers') `
        -Filter '*.dll' -File -ErrorAction SilentlyContinue)
    if ($sqlDrivers.Count -ne 1 -or $sqlDrivers[0].Name -ne 'qsqlite.dll') {
        throw 'The Qt package must contain exactly the SQLite SQL driver.'
    }
    foreach ($dxc in @('dxcompiler.dll', 'dxil.dll')) {
        if (Test-Path -LiteralPath (Join-Path $AppDirectory $dxc)) {
            throw "Unexpected DXC file in the D3D11 payload: $dxc"
        }
    }

    foreach ($entry in $files.GetEnumerator()) {
        $staged = Join-Path $AppDirectory $entry.Key
        if (-not (Test-Path -LiteralPath $staged -PathType Leaf)) {
            throw "Missing packaged runtime input: $($entry.Key)"
        }
        $sha256 = (Get-FileHash -LiteralPath $staged).Hash
        if ($sha256 -ne (Get-FileHash -LiteralPath $entry.Value).Hash) {
            throw "Packaged runtime differs from the selected toolchain: $($entry.Key)"
        }
        [ordered]@{ path = $entry.Key.Replace('\', '/'); sha256 = $sha256.ToLowerInvariant() }
    }
}
