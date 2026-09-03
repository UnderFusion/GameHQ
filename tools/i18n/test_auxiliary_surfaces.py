#!/usr/bin/env python3
"""Verify that every non-Qt runtime and tool has a localization boundary."""

from __future__ import annotations

import fnmatch
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
INVENTORY = ROOT / "i18n" / "surface-inventory.json"
BOUNDARY_DOC = ROOT / "docs" / "localization" / "auxiliary-surfaces.md"
ALLOWED = {
    "localized now",
    "developer/log-only",
    "English-by-policy",
    "deferred follow-up",
}
REQUIRED_FIELDS = {
    "id",
    "runtime_build_system",
    "packaging_boundary",
    "user_visible",
    "locale_source",
    "native_mechanism",
    "fallback",
    "classification",
    "rationale",
    "coverage",
}


def fail(message: str) -> None:
    raise AssertionError(message)


def covered(path: str, boundaries: list[dict]) -> bool:
    return any(
        fnmatch.fnmatchcase(path, pattern)
        for boundary in boundaries
        for pattern in boundary["coverage"]
    )


def discover_script_surfaces() -> set[str]:
    paths: set[str] = set()
    for pattern in (
        "packaging/*.ps1",
        "integrations/playnite/packaging/*.ps1",
        "tools/**/*.py",
        "tools/**/*.ps1",
        ".github/workflows/*.yml",
        ".github/workflows/*.yaml",
    ):
        paths.update(path.relative_to(ROOT).as_posix() for path in ROOT.glob(pattern))
    return paths


def discover_document_surfaces() -> set[str]:
    paths: set[str] = set()
    for pattern in (
        "packaging/*.md",
        "packaging/*.txt",
        "integrations/playnite/*.md",
        "integrations/playnite/*.yaml",
        "integrations/playnite/LICENSE",
        "integrations/playnite/LICENSES/**/*",
        "tools/manual-validation/*.md",
    ):
        paths.update(
            path.relative_to(ROOT).as_posix()
            for path in ROOT.glob(pattern)
            if path.is_file()
        )
    return paths


def main() -> int:
    data = json.loads(INVENTORY.read_text(encoding="utf-8"))
    boundaries = data.get("auxiliary_boundaries", [])
    ids = {boundary.get("id") for boundary in boundaries}
    expected_ids = {
        "playnite-plugin-runtime",
        "win32-launcher",
        "updater-helper-process",
        "inno-installer",
        "gameinput-runtime-probe",
        "release-manifest-tool",
        "automation-scripts",
        "packaged-documentation",
    }
    if ids != expected_ids:
        fail(f"auxiliary boundary IDs differ: expected {sorted(expected_ids)}, got {sorted(ids)}")

    doc = BOUNDARY_DOC.read_text(encoding="utf-8")
    for boundary in boundaries:
        missing = REQUIRED_FIELDS - boundary.keys()
        if missing:
            fail(f"{boundary.get('id', '<unknown>')} misses fields: {sorted(missing)}")
        if boundary["classification"] not in ALLOWED:
            fail(f"{boundary['id']} has unsupported classification {boundary['classification']!r}")
        row = next((line for line in doc.splitlines() if f"| `{boundary['id']}` |" in line), "")
        if f"`{boundary['classification']}`" not in row:
            fail(f"{boundary['id']} is not documented with its classification")
        if boundary["classification"] == "deferred follow-up":
            for field in ("owner", "scope", "acceptance"):
                if not boundary.get(field):
                    fail(f"{boundary['id']} deferred work misses {field}")

    cmake = (ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
    targets = set(re.findall(r"(?:qt_)?add_executable\(\s*([A-Za-z0-9_]+)", cmake))
    auxiliary_targets = targets - {"GameHQ"}
    mapped_targets = {
        target for boundary in boundaries for target in boundary.get("build_targets", [])
    }
    if auxiliary_targets != mapped_targets:
        fail(
            "non-Qt executable target coverage differs: "
            f"expected {sorted(auxiliary_targets)}, got {sorted(mapped_targets)}"
        )

    project_files = {
        path.relative_to(ROOT).as_posix()
        for base in (ROOT / "integrations" / "playnite" / "src", ROOT / "tools")
        for path in base.rglob("*.csproj")
        # bootstrap-inno.ps1 installs the pinned third-party compiler under
        # tools/InnoSetup; its bundled examples are not GameHQ-owned tools.
        if not path.relative_to(ROOT).as_posix().startswith("tools/InnoSetup/")
    }
    mapped_projects = {
        project for boundary in boundaries for project in boundary.get("build_manifests", [])
    }
    if project_files != mapped_projects:
        fail(f".NET project coverage differs: expected {sorted(project_files)}, got {sorted(mapped_projects)}")

    discovered_files = discover_script_surfaces() | discover_document_surfaces() | {
        "packaging/GameHQ.iss"
    }
    omitted = sorted(path for path in discovered_files if not covered(path, boundaries))
    if omitted:
        fail(f"unclassified auxiliary files: {omitted}")

    launcher = (ROOT / "src" / "launcher" / "LauncherMain.cpp").read_text(encoding="utf-8")
    launcher_localization = (
        ROOT / "src" / "launcher" / "LauncherLocalization.cpp"
    ).read_text(encoding="utf-8")
    launcher_resources = (
        ROOT / "src" / "launcher" / "LauncherStrings.rc"
    ).read_text(encoding="utf-8")
    if launcher.count("showMessage(") != 6 or launcher.count("MessageBoxW(") != 1:
        fail("launcher dialog surface changed; update the boundary inventory")
    if "LoadStringW" not in launcher_localization:
        fail("launcher dialogs must load native Win32 string resources")
    if launcher_resources.count("STRINGTABLE") != 16:
        fail("launcher must embed one STRINGTABLE for every production locale")
    if "qtTrId" in launcher or "qsTrId" in launcher or "Qt" in launcher_localization:
        fail("the static Win32 launcher must not depend on Qt catalogs")

    updater = (ROOT / "src" / "updater" / "UpdaterMain.cpp").read_text(encoding="utf-8")
    if "MessageBox" in updater or "ShowErrorMessage" in updater:
        fail("the detached updater helper is no longer log-only")

    plugin_root = ROOT / "integrations" / "playnite" / "src" / "GameHQ.Playnite"
    plugin_sources = "\n".join(
        path.read_text(encoding="utf-8")
        for pattern in ("*.cs", "*.xaml")
        for path in plugin_root.rglob(pattern)
    )
    for marker in (
        "DynamicResource LOCGameHQIntegrationName",
        'Strings.Get("LOCGameHQIntegrationOpenGameHQ")',
        "ShowErrorMessage(",
    ):
        if marker not in plugin_sources:
            fail(f"Playnite user-facing marker changed or disappeared: {marker}")

    installer = (ROOT / "packaging" / "GameHQ.iss").read_text(encoding="utf-8")
    installer_languages = (
        ROOT / "packaging" / "generated" / "InnoLanguages.iss"
    ).read_text(encoding="utf-8")
    if (
        "[Messages]" not in installer
        or '#include "generated\\InnoLanguages.iss"' not in installer
        or installer_languages.count('Name: "') != 16
    ):
        fail("installer language mapping changed; update its boundary classification")

    print(f"auxiliary localization audit passed ({len(boundaries)} boundaries)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError) as error:
        print(f"auxiliary localization audit failed: {error}", file=sys.stderr)
        raise SystemExit(1)
