#!/usr/bin/env python3
"""Focused acceptance tests for the versioned release-note source contract."""

from __future__ import annotations

import copy
import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOT = ROOT / "assets" / "release-notes"
HISTORY_FIXTURE = ROOT / "tools" / "i18n" / "fixtures" / "release-notes-history.en-US.json"
OUTPUT_ROOT = SOURCE_ROOT / "generated"


def load_generator():
    path = ROOT / "tools" / "i18n" / "generate_release_notes.py"
    spec = importlib.util.spec_from_file_location("gamehq_release_notes", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load release-note generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GEN = load_generator()


class ReleaseNotesGenerationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest, self.locales, self.releases = GEN.load_contract(SOURCE_ROOT)

    def temporary_sources(self) -> tuple[tempfile.TemporaryDirectory, Path]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name) / "release-notes"
        shutil.copytree(SOURCE_ROOT, root)
        return temporary, root

    def assert_contract_error(self, callback, needle: str) -> None:
        with self.assertRaises(GEN.ReleaseNotesError) as raised:
            callback()
        self.assertIn(needle, str(raised.exception))

    def test_production_contract_and_generated_bundles_are_current(self) -> None:
        self.assertEqual(2, self.manifest["schema_version"])
        self.assertEqual(16, len(self.locales))
        self.assertEqual(4, len(self.releases))
        GEN.strict_validate_locales(SOURCE_ROOT, self.locales, self.releases)
        GEN.generate_all(SOURCE_ROOT, OUTPUT_ROOT, check=True)
        for release, _ in self.releases:
            version_root = SOURCE_ROOT / "versions" / release["version"]
            self.assertEqual(
                set(self.locales),
                {path.stem for path in version_root.glob("*.json")},
            )
        self.assertEqual(
            {f"release-notes.{locale}.json" for locale in self.locales}
            | {"release-notes.index.json"},
            {path.name for path in OUTPUT_ROOT.glob("*.json")},
        )

    def test_schema_documents_describe_version_two(self) -> None:
        manifest_schema = GEN.read_json(
            ROOT / "i18n" / "schema" / "release-notes-manifest.schema.json"
        )
        document_schema = GEN.read_json(
            ROOT / "i18n" / "schema" / "release-note-document.schema.json"
        )
        self.assertEqual(2, manifest_schema["properties"]["schema_version"]["const"])
        self.assertEqual(3, len(document_schema["oneOf"]))

    def test_generated_english_reproduces_the_frozen_released_history(self) -> None:
        """The released 0.7.3-0.7.6 English history is immutable. The fixture is
        the byte-frozen record migrated off the retired assets/release-notes.json;
        the versioned source must keep reproducing it exactly."""
        frozen = GEN.read_json(HISTORY_FIXTURE)
        generated = GEN.read_json(OUTPUT_ROOT / "release-notes.en-US.json")
        generated.pop("_meta")
        self.assertEqual(frozen, generated)
        self.assertEqual("0.7.6", frozen["version"])
        self.assertEqual("2026-08-31", frozen["date"])
        self.assertEqual(["0.7.5", "0.7.4", "0.7.3"],
                         [release["version"] for release in frozen["history"]])
        for release in [frozen, *frozen["history"]]:
            source = GEN.read_json(SOURCE_ROOT / "versions" / release["version"] / "en-US.json")
            self.assertEqual(source["date"], release["date"])
            self.assertEqual([section["title"] for section in source["sections"]],
                             [section["title"] for section in release["sections"]])
            for section, frozen_section in zip(source["sections"], release["sections"]):
                self.assertEqual([item["text"] for item in section["items"]],
                                 frozen_section["items"])

    def test_the_retired_legacy_source_has_no_remaining_consumer(self) -> None:
        """Repository-wide evidence that nothing still reads the retired file."""
        self.assertFalse((ROOT / "assets" / "release-notes.json").exists())
        this_file = Path(__file__).resolve().relative_to(ROOT).as_posix()
        # Only the two guards and the source README may still name the retired
        # file, and the README may only name it to record that it was retired.
        documented = {this_file, "assets/release-notes/README.md",
                      "tests/tst_releasenotes.cpp"}
        excluded = [":!docs/plans/", ":!out/", ":!out-production/", ":!build/"]
        for needle in ("assets/release-notes.json", ":/release-notes/release-notes.json"):
            found = subprocess.run(
                ["git", "grep", "-lF", needle, "--", *excluded],
                cwd=str(ROOT), capture_output=True, text=True, encoding="utf-8",
            )
            hits = {line for line in found.stdout.splitlines() if line}
            self.assertEqual(hits - documented, set(), f"{needle} still referenced")
        readme = (SOURCE_ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("has been retired", readme)

    def test_generation_is_byte_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            first_root = Path(first)
            second_root = Path(second)
            GEN.generate_all(SOURCE_ROOT, first_root)
            GEN.generate_all(SOURCE_ROOT, second_root)
            first_files = sorted(path.name for path in first_root.glob("*.json"))
            self.assertEqual(first_files, sorted(path.name for path in second_root.glob("*.json")))
            for name in first_files:
                self.assertEqual((first_root / name).read_bytes(), (second_root / name).read_bytes())

    def test_declared_missing_and_invalid_documents_use_whole_english_release(self) -> None:
        english_bundle = GEN.build_bundle(SOURCE_ROOT, "en-US", self.locales, self.releases)
        polish_bundle = GEN.build_bundle(SOURCE_ROOT, "pl-PL", self.locales, self.releases)
        for bundle in (english_bundle, polish_bundle):
            bundle.pop("_meta")
        self.assertEqual(english_bundle, polish_bundle)

        temporary, source_root = self.temporary_sources()
        with temporary:
            polish = source_root / "versions" / "0.7.6" / "pl-PL.json"
            polish.unlink()
            _, locales, releases = GEN.load_contract(source_root)
            missing = GEN.build_bundle(source_root, "pl-PL", locales, releases)
            self.assertEqual("en-US", missing["_meta"]["documents"][0]["resolved_locale"])
            self.assertIn("missing", missing["_meta"]["documents"][0]["fallback_reason"])

            invalid = {
                "$schema": "fixture",
                "schema_version": 2,
                "version": "0.7.6",
                "locale": "pl-PL",
                "date": "2026-08-31",
                "mode": "localized",
                "source_integrity": releases[0][0]["source_integrity"],
                "sections": [{"id": "fixed", "title": "Naprawiono", "items": []}],
            }
            GEN.write_json(polish, invalid)
            fallback = GEN.build_bundle(source_root, "pl-PL", locales, releases)
            self.assertEqual(
                releases[0][1]["sections"][0]["title"],
                fallback["sections"][0]["title"],
            )
            self.assertNotIn("Naprawiono", json.dumps(fallback, ensure_ascii=False))

    def test_duplicate_ids_and_partial_structures_are_rejected(self) -> None:
        english = copy.deepcopy(self.releases[0][1])
        english["sections"][0]["id"] = english["sections"][1]["id"]
        self.assert_contract_error(
            lambda: GEN.validate_english_document(
                english, "0.7.6", "2026-08-31", "duplicate-section"
            ),
            "duplicate section ID",
        )

        english = copy.deepcopy(self.releases[0][1])
        english["sections"][0]["items"][1]["id"] = english["sections"][0]["items"][0]["id"]
        self.assert_contract_error(
            lambda: GEN.validate_english_document(
                english, "0.7.6", "2026-08-31", "duplicate-item"
            ),
            "duplicate item ID",
        )

        localized = self.localized_copy("pl-PL")
        localized["sections"][0]["items"].pop()
        self.assert_contract_error(
            lambda: GEN.validate_locale_document(
                localized, "pl-PL", self.releases[0][1], "missing-item"
            ),
            "missing, extra, or reordered",
        )

        partial = self.localized_copy("pl-PL")
        partial["sections"][0]["items"][0]["extra"] = "mixed"
        self.assert_contract_error(
            lambda: GEN.validate_locale_document(
                partial, "pl-PL", self.releases[0][1], "partial"
            ),
            "partial or mixed item structure",
        )

    def localized_copy(self, locale: str) -> dict:
        english = self.releases[0][1]
        return {
            "$schema": "fixture",
            "schema_version": 2,
            "version": english["version"],
            "locale": locale,
            "date": english["date"],
            "mode": "localized",
            "source_integrity": GEN.source_integrity(english),
            "sections": copy.deepcopy(english["sections"]),
        }

    def test_locale_schema_date_integrity_and_utf8_guards(self) -> None:
        english = self.releases[0][1]
        mutations = (
            ("locale/tag mismatch", lambda value: value.__setitem__("locale", "de-DE")),
            ("unsupported schema version", lambda value: value.__setitem__("schema_version", 1)),
            ("invalid ISO date", lambda value: value.__setitem__("date", "2026-02-30")),
            ("stale source-integrity", lambda value: value.__setitem__("source_integrity", "sha256:" + "0" * 64)),
        )
        for expected, mutate in mutations:
            with self.subTest(expected=expected):
                localized = self.localized_copy("pl-PL")
                mutate(localized)
                self.assert_contract_error(
                    lambda value=localized: GEN.validate_locale_document(
                        value, "pl-PL", english, expected
                    ),
                    expected,
                )

        with tempfile.TemporaryDirectory() as temporary:
            malformed = Path(temporary) / "malformed.json"
            malformed.write_bytes(b'{"text":"\xff"}')
            self.assert_contract_error(lambda: GEN.read_json(malformed), "malformed UTF-8")

    def test_unsupported_and_pseudo_locales_are_rejected(self) -> None:
        for locale in ("cs-CZ", "en-XA", "ar-XB"):
            with self.subTest(locale=locale):
                self.assert_contract_error(
                    lambda locale=locale: GEN.build_bundle(
                        SOURCE_ROOT, locale, self.locales, self.releases
                    ),
                    "unsupported production locale",
                )

    def test_complete_policy_requires_real_documents_for_all_sixteen_locales(self) -> None:
        temporary, source_root = self.temporary_sources()
        with temporary:
            manifest = GEN.read_json(source_root / "manifest.json")
            manifest["releases"][0]["localization_policy"] = "complete"
            GEN.write_json(source_root / "manifest.json", manifest)
            _, locales, releases = GEN.load_contract(source_root)
            self.assert_contract_error(
                lambda: GEN.strict_validate_locales(source_root, locales, releases),
                "complete releases cannot use English fallback",
            )

    def test_released_history_requires_explicit_correction_metadata(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["releases"][1]["source_integrity"] = "sha256:" + "1" * 64
        self.assert_contract_error(
            lambda: GEN.validate_manifest(manifest, self.locales),
            "without explicit correction metadata",
        )

    def test_stale_manifest_integrity_is_rejected(self) -> None:
        temporary, source_root = self.temporary_sources()
        with temporary:
            manifest = GEN.read_json(source_root / "manifest.json")
            manifest["releases"][0]["source_integrity"] = "sha256:" + "2" * 64
            manifest["releases"][0]["original_source_integrity"] = "sha256:" + "2" * 64
            GEN.write_json(source_root / "manifest.json", manifest)
            self.assert_contract_error(
                lambda: GEN.load_contract(source_root),
                "stale manifest source-integrity metadata",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
