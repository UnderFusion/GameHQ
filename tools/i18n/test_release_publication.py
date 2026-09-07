#!/usr/bin/env python3
"""Focused validation for the deterministic release-publication generator (p6-4)."""

from __future__ import annotations

import copy
import hashlib
import json
import re
import shutil
import tempfile
import unittest
from pathlib import Path

import generate_release_notes as source_tool
import generate_release_publication as publication


ROOT = publication.ROOT
SOURCE_ROOT = ROOT / "assets" / "release-notes"
COMMITTED_ROOT = publication.DEFAULT_OUTPUT_ROOT
LINK = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def read_manifest() -> dict:
    return read_json(SOURCE_ROOT / "manifest.json")

MANIFEST = read_manifest()
# The newest release and the newest English-fallback release are manifest
# facts; pinning them would tie the suite to one point in the history.
CURRENT_VERSION = MANIFEST["releases"][0]["version"]
FALLBACK_VERSION = next(release["version"] for release in MANIFEST["releases"]
                        if release["localization_policy"] == "fallback-allowed")


class PublicationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.workspace = Path(self.temp.name)

    def copied_source(self, name: str = "source") -> Path:
        target = self.workspace / name
        shutil.copytree(SOURCE_ROOT, target, ignore=shutil.ignore_patterns("publication"))
        return target

    def generate(self, name: str, source_root: Path | None = None) -> Path:
        output = self.workspace / name
        publication.publish(source_root or SOURCE_ROOT, output, None, False)
        return output

    def files(self, root: Path) -> dict[str, bytes]:
        return {
            path.relative_to(root).as_posix(): path.read_bytes()
            for path in sorted(root.rglob("*")) if path.is_file()
        }

    def test_one_command_regenerates_the_complete_set_byte_identically(self) -> None:
        first = self.files(self.generate("first"))
        second = self.files(self.generate("second"))
        self.assertEqual(first, second)
        self.assertEqual(first, self.files(COMMITTED_ROOT))
        versions = {name.split("/", 1)[0] for name in first}
        manifest = read_json(SOURCE_ROOT / "manifest.json")
        self.assertEqual(versions, {r["version"] for r in manifest["releases"]})
        for version in versions:
            names = {name.split("/", 1)[1] for name in first if name.startswith(f"{version}/")}
            self.assertEqual(len(names), 18)
            self.assertIn(publication.BODY_FILENAME, names)
            self.assertIn(publication.METADATA_FILENAME, names)

    def test_every_production_locale_appears_exactly_once_with_canonical_names(self) -> None:
        expected = read_json(ROOT / "i18n" / "locales.json")
        production = [entry["tag"] for entry in expected["locales"]
                      if entry.get("state") == "enabled" and entry.get("tier") == 1]
        self.assertEqual(len(production), 16)
        for version_dir in sorted(COMMITTED_ROOT.iterdir()):
            metadata = read_json(version_dir / publication.METADATA_FILENAME)
            locales = [asset["canonical_locale"] for asset in metadata["localized_documents"]]
            filenames = [asset["filename"] for asset in metadata["localized_documents"]]
            self.assertEqual(locales, production)
            self.assertEqual(len(set(locales)), 16)
            self.assertEqual(len(set(filenames)), 16)
            for asset in metadata["localized_documents"]:
                self.assertEqual(asset["filename"],
                                 f"release-notes.{asset['canonical_locale']}.md")
                self.assertNotIn(asset["canonical_locale"], source_tool.PSEUDO_LOCALES)
        # Regional and script variants stay distinct publication assets.
        for pair in (("es-ES", "es-419"), ("zh-Hans", "zh-Hant"), ("pt-BR", "es-419")):
            left = (COMMITTED_ROOT / CURRENT_VERSION / f"release-notes.{pair[0]}.md")
            right = (COMMITTED_ROOT / CURRENT_VERSION / f"release-notes.{pair[1]}.md")
            self.assertTrue(left.is_file() and right.is_file())
            self.assertNotEqual(left.read_bytes(), right.read_bytes())

    def test_size_sha256_and_source_integrity_match_the_generated_bytes(self) -> None:
        manifest = {release["version"]: release
                    for release in read_json(SOURCE_ROOT / "manifest.json")["releases"]}
        for version_dir in sorted(COMMITTED_ROOT.iterdir()):
            metadata = read_json(version_dir / publication.METADATA_FILENAME)
            release = manifest[metadata["version"]]
            self.assertEqual(metadata["release_source_integrity"], release["source_integrity"])
            self.assertEqual(metadata["date"], release["date"])
            self.assertEqual(metadata["localization_policy"], release["localization_policy"])
            english = read_json(SOURCE_ROOT / "versions" / metadata["version"] / "en-US.json")
            self.assertEqual(source_tool.source_integrity(english),
                             metadata["release_source_integrity"])
            records = list(metadata["localized_documents"]) + [metadata["release_body"]]
            for record in records:
                payload = (version_dir / record["filename"]).read_bytes()
                self.assertEqual(record["size"], len(payload))
                self.assertEqual(record["sha256"], hashlib.sha256(payload).hexdigest())
            for asset in metadata["localized_documents"]:
                self.assertEqual(asset["source_integrity"], release["source_integrity"])
                self.assertEqual(asset["version"], metadata["version"])

    def test_every_published_release_matches_its_declared_policy(self) -> None:
        """A complete release publishes real translations; a fallback-allowed one
        publishes English to every locale. Which releases exist is history."""
        policies = {release["version"]: release["localization_policy"]
                    for release in MANIFEST["releases"]}
        for version_dir in sorted(COMMITTED_ROOT.iterdir()):
            metadata = read_json(version_dir / publication.METADATA_FILENAME)
            version = metadata["version"]
            self.assertEqual(policies[version], metadata["localization_policy"])
            complete = metadata["localization_policy"] == "complete"
            for asset in metadata["localized_documents"]:
                with self.subTest(version=version, locale=asset["canonical_locale"]):
                    text = (version_dir / asset["filename"]).read_text(encoding="utf-8")
                    self.assertEqual(asset["requested_locale"], asset["canonical_locale"])
                    if complete or asset["canonical_locale"] == "en-US":
                        self.assertEqual(asset["effective_locale"], asset["canonical_locale"])
                        self.assertFalse(asset["fallback"])
                        self.assertIsNone(asset["fallback_reason"])
                        self.assertEqual(asset["fallback_state"], "authored")
                        self.assertNotIn("No reviewed", text)
                    else:
                        self.assertEqual(asset["effective_locale"], "en-US")
                        self.assertTrue(asset["fallback"])
                        self.assertEqual(asset["fallback_state"], "whole-document-fallback")
                        self.assertIn("No reviewed", text)

    def test_fallback_assets_separate_requested_from_effective_locale(self) -> None:
        version_dir = COMMITTED_ROOT / FALLBACK_VERSION
        metadata = read_json(version_dir / publication.METADATA_FILENAME)
        english = (version_dir / "release-notes.en-US.md").read_text(encoding="utf-8")
        for asset in metadata["localized_documents"]:
            text = (version_dir / asset["filename"]).read_text(encoding="utf-8")
            self.assertEqual(asset["requested_locale"], asset["canonical_locale"])
            if asset["canonical_locale"] == "en-US":
                self.assertFalse(asset["fallback"])
                self.assertIsNone(asset["fallback_reason"])
                self.assertEqual(asset["fallback_state"], "authored")
                self.assertNotIn("> This document is the original", text)
                continue
            self.assertTrue(asset["fallback"])
            self.assertEqual(asset["effective_locale"], "en-US")
            self.assertEqual(asset["fallback_state"], "whole-document-fallback")
            self.assertEqual(asset["fallback_reason"], "declared-en-US-fallback")
            self.assertIn("No reviewed", text)
            self.assertIn(asset["language_english_name"], text)
            # Whole-document fallback: an English notice is the only difference,
            # so no field of this document can be part translated.
            notice = [line for line in text.split("\n") if line.startswith("> ")]
            self.assertEqual(len(notice), 1)
            stripped = [line for line in text.split("\n") if not line.startswith("> ")]
            collapsed = re.sub(r"\n{3,}", "\n\n", "\n".join(stripped)).strip()
            self.assertEqual(collapsed, english.strip())

    def test_localized_documents_are_never_field_level_mixed(self) -> None:
        source_root = self.copied_source()
        english = read_json(source_root / "versions" / CURRENT_VERSION / "en-US.json")
        localized = {
            "$schema": "../../../../i18n/schema/release-note-document.schema.json",
            "schema_version": 2,
            "version": CURRENT_VERSION,
            "locale": "pl-PL",
            "date": english["date"],
            "mode": "localized",
            "source_integrity": source_tool.source_integrity(english),
            "sections": [
                {
                    "id": section["id"],
                    "title": f"PL {section['title']}",
                    "items": [{"id": item["id"], "text": f"PL {item['text']}"}
                              for item in section["items"]],
                }
                for section in english["sections"]
            ],
        }
        path = source_root / "versions" / CURRENT_VERSION / "pl-PL.json"
        path.write_text(json.dumps(localized, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8", newline="\n")
        output = self.generate("localized", source_root)
        text = (output / CURRENT_VERSION / "release-notes.pl-PL.md").read_text(encoding="utf-8")
        metadata = read_json(output / CURRENT_VERSION / publication.METADATA_FILENAME)
        asset = next(a for a in metadata["localized_documents"] if a["canonical_locale"] == "pl-PL")
        self.assertFalse(asset["fallback"])
        self.assertEqual(asset["effective_locale"], "pl-PL")
        self.assertEqual(asset["fallback_state"], "authored")
        self.assertNotIn("> This document is the original", text)
        parsed = publication.parse_document(text, "pl-PL")
        self.assertEqual(parsed, publication.structured_meaning(localized))
        # Every rendered line of a localized document comes from the localized
        # document only, so no English section or item can leak into it.
        for section in parsed["sections"]:
            self.assertTrue(section["title"].startswith("PL "))
            self.assertTrue(all(item.startswith("PL ") for item in section["items"]))
        self.assertEqual(asset["structure"],
                         [{"section_id": s["id"], "item_ids": [i["id"] for i in s["items"]]}
                          for s in localized["sections"]])

    def test_pseudo_and_unsupported_locales_are_rejected(self) -> None:
        production = read_json(SOURCE_ROOT / "manifest.json")["production_locales"]
        for bad in ("en-XA", "ar-XB"):
            with self.assertRaises(source_tool.ReleaseNotesError):
                publication.validate_publication_locales(production[:-1] + [bad], production)
        with self.assertRaises(source_tool.ReleaseNotesError):
            publication.validate_publication_locales(production[:-1] + ["xx-YY"], production)
        with self.assertRaises(source_tool.ReleaseNotesError):
            publication.validate_publication_locales(production + ["pl-PL"], production)
        with self.assertRaises(source_tool.ReleaseNotesError):
            publication.validate_publication_locales(production[:-1], production)
        with self.assertRaises(source_tool.ReleaseNotesError):
            publication.publish(SOURCE_ROOT, self.workspace / "none", "9.9.9", False)

    def test_missing_or_unexpected_publication_assets_fail_the_check(self) -> None:
        output = self.generate("checked")
        publication.publish(SOURCE_ROOT, output, None, True)
        removed = output / CURRENT_VERSION / "release-notes.ja-JP.md"
        payload = removed.read_bytes()
        removed.unlink()
        with self.assertRaises(source_tool.ReleaseNotesError):
            publication.publish(SOURCE_ROOT, output, None, True)
        removed.write_bytes(payload)
        stray = output / CURRENT_VERSION / "release-notes.ja-JP.copy.md"
        stray.write_bytes(payload)
        with self.assertRaises(source_tool.ReleaseNotesError):
            publication.publish(SOURCE_ROOT, output, None, True)
        stray.unlink()
        removed.write_bytes(payload + b"drift\n")
        with self.assertRaises(source_tool.ReleaseNotesError):
            publication.publish(SOURCE_ROOT, output, None, True)

    def test_content_links_version_and_date_match_the_structured_source(self) -> None:
        for version_dir in sorted(COMMITTED_ROOT.iterdir()):
            version = version_dir.name
            english = read_json(SOURCE_ROOT / "versions" / version / "en-US.json")
            for asset in read_json(version_dir / publication.METADATA_FILENAME)["localized_documents"]:
                source = read_json(
                    SOURCE_ROOT / "versions" / version / f"{asset['effective_locale']}.json")
                expected = publication.structured_meaning(source)
                text = (version_dir / asset["filename"]).read_text(encoding="utf-8")
                parsed = publication.parse_document(text, asset["filename"])
                self.assertEqual(parsed["version"], version)
                self.assertEqual(parsed["date"], english["date"])
                self.assertEqual(parsed, expected)
                authored = "\n".join(
                    [section["title"] for section in expected["sections"]]
                    + [item for section in expected["sections"] for item in section["items"]])
                self.assertEqual(sorted(LINK.findall(text)), sorted(LINK.findall(authored)))

    def test_release_body_links_the_repository_not_per_locale_release_assets(self) -> None:
        """Per-locale notes are repository documents, never individual release assets."""
        for version_dir in sorted(COMMITTED_ROOT.iterdir()):
            metadata = read_json(version_dir / publication.METADATA_FILENAME)
            version = metadata["version"]
            body = (version_dir / publication.BODY_FILENAME).read_text(encoding="utf-8")
            english = read_json(SOURCE_ROOT / "versions" / version / "en-US.json")
            self.assertEqual(publication.body_release_notes(body, "body"),
                             publication.structured_meaning(english))
            expected_url = publication.LOCALIZED_NOTES_URL.format(version=version)
            targets = {target for _, target in LINK.findall(body)}
            self.assertIn(expected_url, targets)
            for document in metadata["localized_documents"]:
                self.assertNotIn(document["filename"], targets)
                self.assertTrue((version_dir / document["filename"]).is_file())
            self.assertNotIn(publication.LOCALE_DOCUMENT_LINK, body)
            self.assertEqual(metadata["distribution"]["github_release_assets"], False)
            self.assertEqual(metadata["distribution"]["localized_documents"], "repository")
            self.assertEqual(metadata["distribution"]["localized_documents_url"], expected_url)
            self.assertIn("published in English by default", body)
            self.assertIn("are kept in the", body)
            self.assertNotIn("en-XA", body)
            self.assertNotIn("ar-XB", body)

    def test_malformed_utf8_and_unpublishable_text_fail_deterministically(self) -> None:
        broken = self.copied_source("broken-utf8")
        (broken / "versions" / CURRENT_VERSION / "en-US.json").write_bytes(b'{"a": "\xff\xfe"}')
        messages = []
        for _ in range(2):
            with self.assertRaises(source_tool.ReleaseNotesError) as caught:
                publication.publish(broken, self.workspace / "broken-out", None, False)
            messages.append(str(caught.exception))
        self.assertEqual(messages[0], messages[1])
        self.assertIn("malformed UTF-8 or JSON", messages[0])

        for bad_text in ("line\nbreak", "trailing ", "tab\tstop"):
            unpublishable = copy.deepcopy(
                read_json(SOURCE_ROOT / "versions" / CURRENT_VERSION / "en-US.json"))
            unpublishable["sections"][0]["items"][0]["text"] = bad_text
            with self.assertRaises(source_tool.ReleaseNotesError):
                publication.render_document(unpublishable, "en-US", "en-US",
                                            {"en-US": {"english": "English", "native": "English"}})

    def test_publication_output_is_not_an_update_or_runtime_trust_input(self) -> None:
        haystacks = list((ROOT / "src").rglob("*.cpp")) + list((ROOT / "src").rglob("*.h"))
        haystacks += list((ROOT / "packaging").glob("*.ps1"))
        haystacks += [ROOT / "src" / "CMakeLists.txt", ROOT / "CMakeLists.txt"]
        for path in haystacks:
            text = path.read_text(encoding="utf-8", errors="replace")
            self.assertNotIn("release-notes/publication", text, str(path))
            self.assertNotIn("publication-metadata", text, str(path))
        changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertNotIn("gamehq:locale-index", changelog)


if __name__ == "__main__":
    unittest.main()
