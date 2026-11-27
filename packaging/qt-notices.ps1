# The assembler and release gate use the same expected SPDX inventory.
function Get-GameHQQtSbom {
    param($Qt)

    $modules = @('qtbase', 'qtdeclarative', 'qtmultimedia', 'qtsvg', 'qttranslations')
    $imageFormats = Join-Path $Qt.Prefix 'plugins\imageformats'
    if (@(Get-ChildItem -LiteralPath $imageFormats -File -ErrorAction SilentlyContinue |
            Where-Object Name -in @('qicns.dll', 'qtga.dll', 'qtiff.dll', 'qwbmp.dll', 'qwebp.dll')).Count) {
        $modules += 'qtimageformats'
    }

    foreach ($module in $modules) {
        $name = "$module-$($Qt.Version)"
        $path = Join-Path $Qt.Prefix "sbom\$name.spdx"
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Content -LiteralPath $path -Raw) -notmatch "(?m)^DocumentName: $([regex]::Escape($name))\r?$") {
            throw "Missing or mismatched Qt SPDX document: $path"
        }
        Get-Item -LiteralPath $path
    }
}

function Get-GameHQPackageNotices {
    param([string]$Root, $Qt)

    $notices = Get-Content -LiteralPath (Join-Path $Root 'THIRD_PARTY_NOTICES.md') -Raw
    $notices = $notices -replace '(?m)^## Qt(?=\r?$)', "## Qt $($Qt.Version)"
    $multimedia = Get-Content -LiteralPath (Join-Path $Qt.Prefix "sbom\qtmultimedia-$($Qt.Version).spdx") -Raw
    $ffmpeg = [regex]::Match($multimedia,
        '(?m)^PackageName: FFmpeg\r?\n(?:(?!PackageName:)[^\r\n]+\r?\n)*?PackageVersion: (\d+(?:\.\d+)+)\r?$')
    if ($ffmpeg.Success) {
        $notices = $notices -replace '(?m)^## FFmpeg(?=\r?$)', "## FFmpeg $($ffmpeg.Groups[1].Value)"
    }
    return $notices
}
