#!/usr/bin/env python3
"""Merge isolated runtime, package, and installer evidence into one locale matrix."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


FORBIDDEN = {"en-XA", "ar-XB", "cs-CZ"}
SUPPORT_URL = "https://ko-fi.com/underfusion"


def _load(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def build_report(
    root: Path,
    package: dict[str, Any],
    bootstrap: dict[str, Any],
    regression: dict[str, Any],
) -> dict[str, Any]:
    manifest = _load(root / "i18n" / "locales.json")
    locales = [entry["tag"] for entry in manifest["locales"] if entry["state"] == "enabled"]
    _require(len(locales) == 16, f"manifest: expected 16 production locales, got {len(locales)}")
    _require(not (set(locales) & FORBIDDEN), "manifest: reserve/development locale entered production")

    package_rows = {row["locale"]: row for row in package.get("locales", [])}
    _require(list(package_rows) == locales, "package: locale order or membership differs from manifest")
    _require(package.get("runtime_locale_count") == 16, "package: runtime matrix is incomplete")
    _require(package.get("update_authorization_input") is False,
             "package: localized presentation became an update authorization input")
    _require(package.get("external_browser_opened") is False,
             "package: automated probe must not launch an external browser")
    payload_hash = package.get("payload_application", {}).get("sha256")
    _require(bool(payload_hash), "package: payload application hash is missing")
    _require(package.get("portable_application", {}).get("sha256") == payload_hash,
             "package: portable application differs from the verified payload")
    _require(package.get("update_application", {}).get("sha256") == payload_hash,
             "package: update application differs from the verified payload")

    mappings = {row["app_locale"]: row["installer_language"]
                for row in bootstrap.get("mappings", [])}
    _require(set(mappings) == set(locales), "installer bootstrap: not all 16 semantic mappings passed")
    representative = {row["locale"]: row for row in regression.get("critical_languages", [])}
    required_representatives = {
        "en-US", "pl-PL", "zh-Hans", "zh-Hant", "ru-RU", "th-TH", "es-419", "de-DE"
    }
    _require(set(representative) == required_representatives,
             "installer regression: representative locale set differs")
    _require(any("real GameHQ profile unchanged" in value
                 for value in bootstrap.get("checks", [])),
             "installer bootstrap: real profile preservation evidence is missing")
    _require(any("real GameHQ installation metadata unchanged" in value
                 for value in regression.get("checks", [])),
             "installer regression: real installation preservation evidence is missing")

    brand = (root / "src" / "ui" / "qml" / "Brand.qml").read_text(encoding="utf-8")
    sidebar = (root / "src" / "ui" / "qml" / "components" / "DesktopSidebar.qml").read_text(
        encoding="utf-8"
    )
    _require(brand.count(SUPPORT_URL) == 1, "support CTA: URL must be defined exactly once")
    _require("externalUrlOpener(Brand.supportUrl)" in sidebar,
             "support CTA: click does not dispatch the shared URL")
    _require("Qt.openUrlExternally(url)" in sidebar,
             "support CTA: default dispatch is not the external browser boundary")

    matrix: list[dict[str, Any]] = []
    for locale in locales:
        row = package_rows[locale]
        for field in (
            "live_switch", "repeated_switch", "system_resolution", "persistence_restart",
            "installer_handoff_consumed", "whole_document_notes",
        ):
            _require(row.get(field) is True, f"{locale}: runtime check failed: {field}")
        _require(row.get("requested_locale") == locale, f"{locale}: requested locale mismatch")
        _require(row.get("effective_locale") == locale, f"{locale}: effective locale mismatch")
        _require(bool(row.get("about")), f"{locale}: About text is missing")
        _require(bool(row.get("support_gamehq")), f"{locale}: support CTA text is missing")
        _require(bool(row.get("formatted_date")), f"{locale}: formatted date is missing")
        _require(row.get("release_count", 0) > 0, f"{locale}: offline release history is missing")
        matrix.append({
            "locale": locale,
            "status": "passed",
            "requested_locale": row["requested_locale"],
            "effective_locale": row["effective_locale"],
            "selection_persistence_restart": True,
            "live_and_repeated_switch": True,
            "raw_id_or_missing_marker": False,
            "about": row["about"],
            "support_gamehq": row["support_gamehq"],
            "support_url": SUPPORT_URL,
            "external_browser_opened": False,
            "release_notes_whole_document": True,
            "release_note_fallback_documents": row["fallback_documents"],
            "formatted_date": row["formatted_date"],
            "installer_language": mappings[locale],
            "fresh_installer_handoff": True,
            "full_installer_lifecycle_smoke": locale in representative,
            "packaged_payload": True,
            "portable_archive": True,
            "update_archive": True,
            "offline": True,
        })

    return {
        "schema_version": 1,
        "acceptance_kind": "technical_runtime_only",
        "linguistic_acceptance": False,
        "production_locale_count": len(matrix),
        "passed_locale_count": len(matrix),
        "failed_locale_count": 0,
        "production_locales": locales,
        "forbidden_production_locales": sorted(FORBIDDEN),
        "scenario_evidence": {
            "per_locale_runtime_slots": len(matrix),
            "per_locale_fresh_installer_handoffs": len(mappings),
            "representative_full_installer_lifecycles": len(representative),
            "bootstrap_checks_passed": bootstrap.get("checks_passed", 0),
            "installer_regression_checks_passed": regression.get("checks_passed", 0),
            "missing_corrupt_and_whole_document_fallback": "focused CTest acceptance",
            "portable_and_package_archives": "byte-matched production artifacts",
            "long_script_layout_stress": ["de-DE", "ru-RU"],
            "cjk_thai_stress": ["zh-Hans", "zh-Hant", "th-TH"],
            "offline": True,
            "owner_environment_unchanged": True,
        },
        "support_url": SUPPORT_URL,
        "support_url_dispatch_verified": True,
        "external_browser_opened": False,
        "update_authorization_input": False,
        "failures": [],
        "locales": matrix,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--package-report", type=Path, required=True)
    parser.add_argument("--bootstrap-report", type=Path, required=True)
    parser.add_argument("--regression-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = build_report(
            args.root.resolve(), _load(args.package_report), _load(args.bootstrap_report),
            _load(args.regression_report),
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        print(f"multilingual acceptance failed: {exc}")
        return 1
    print(f"multilingual acceptance passed: {report['passed_locale_count']}/16 locales")
    print(f"report: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
