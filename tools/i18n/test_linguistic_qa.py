#!/usr/bin/env python3
"""Focused tests for contextual linguistic-review evidence."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
import linguistic_qa
import check_diff
import sync as extraction
import verify


ROOT = Path(__file__).resolve().parents[2]
REVIEWS = ROOT / "i18n/quality/reviews"
ARTIFACT = REVIEWS / "pl-PL.json"


class LinguisticReviewTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.expected_ids = set(linguistic_qa.source_messages(ROOT))

    def test_reviewed_surfaces_use_repository_lf_bytes(self) -> None:
        # Evidence hashes must survive Git's LF checkout normalization.
        # CRLF or mixed local files previously passed locally but failed CI.
        for artifact in sorted(REVIEWS.glob("*.json")):
            review = json.loads(artifact.read_text(encoding="utf-8"))
            for surface in review["surfaces"]:
                with self.subTest(locale=artifact.stem, path=surface["path"]):
                    payload = (ROOT / surface["path"]).read_bytes()
                    self.assertNotIn(b"\r", payload)

    def test_every_completed_locale_review_validates(self) -> None:
        artifacts = sorted(REVIEWS.glob("*.json"))
        self.assertTrue(artifacts)
        for artifact in artifacts:
            with self.subTest(artifact=artifact.name):
                review = linguistic_qa.validate_review(ROOT, artifact)
                self.assertEqual(artifact.stem, review["locale"])
                self.assertCountEqual(self.expected_ids, [entry["id"] for entry in review["coverage"]])
                self.assertEqual([], review["unresolved"])

    def test_polish_review_has_complete_auditable_coverage(self) -> None:
        review = linguistic_qa.validate_review(ROOT, ARTIFACT)
        self.assertCountEqual(self.expected_ids, [entry["id"] for entry in review["coverage"]])
        self.assertTrue(review["corrections"])
        self.assertEqual([], review["unresolved"])
        self.assertEqual("agent", review["review"]["reviewer_kind"])
        self.assertEqual("contextually_reviewed", review["review"]["state"])

    def test_stale_hash_missing_id_and_private_evidence_are_rejected(self) -> None:
        original = json.loads(ARTIFACT.read_text(encoding="utf-8"))
        cases = []
        stale = copy.deepcopy(original)
        stale["reviewed"]["sha256"] = "0" * 64
        cases.append((stale, "catalog identity, hash, or count is stale"))
        missing = copy.deepcopy(original)
        missing["coverage"].pop()
        cases.append((missing, f"coverage must contain exactly {len(self.expected_ids)}"))
        duplicate = copy.deepcopy(original)
        duplicate["coverage"][-1] = copy.deepcopy(duplicate["coverage"][0])
        cases.append((duplicate, "coverage IDs do not exactly match"))
        wrong_id = copy.deepcopy(original)
        wrong_id["coverage"][-1]["id"] = "gamehq.fixture.unexpected"
        cases.append((wrong_id, "coverage IDs do not exactly match"))
        stale_source = copy.deepcopy(original)
        stale_source["coverage"][0]["source_hash"] = "0" * 64
        cases.append((stale_source, "stale reviewed source hash"))
        stale_translation = copy.deepcopy(original)
        stale_translation["coverage"][0]["translation_hash"] = "0" * 64
        cases.append((stale_translation, "stale reviewed translation hash"))
        private = copy.deepcopy(original)
        private["review"]["telemetry"] = "forbidden"
        cases.append((private, "forbidden private field"))
        unresolved = copy.deepcopy(original)
        unresolved["unresolved"] = ["defect"]
        cases.append((unresolved, "unresolved linguistic defects"))
        for value, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic), tempfile.TemporaryDirectory() as temp:
                path = Path(temp) / "review.json"
                path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
                with self.assertRaisesRegex(linguistic_qa.ReviewError, diagnostic):
                    linguistic_qa.validate_review(ROOT, path)


class ManifestEvolutionTest(unittest.TestCase):
    """Exercise real sync/reconciliation against isolated copies of accepted evidence."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="gamehq-review-evolution-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.original_review = (REVIEWS / "fr-FR.json").read_bytes()
        self.review = json.loads(self.original_review)
        paths = {
            "i18n/locales.json", "i18n/extracted/messages.json",
            "i18n/state/translations.json", "i18n/quality/reviews/fr-FR.json",
            "assets/release-notes/linguistic-state.json", "src/ui/qml/Brand.qml",
            *(surface["path"] for surface in self.review["surfaces"]),
        }
        for relative in paths:
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        shutil.copytree(ROOT / "i18n/app", self.root / "i18n/app", dirs_exist_ok=True)
        self.manifest_path = self.root / "i18n/extracted/messages.json"
        self.manifest = linguistic_qa.read_json(self.manifest_path)
        self.base = {entry["id"]: copy.deepcopy(entry) for entry in self.manifest["messages"]}
        self.registry = linguistic_qa.read_json(self.root / "i18n/locales.json")
        self.prior_state = linguistic_qa.read_json(self.root / "i18n/state/translations.json")
        self.catalog_path = self.root / self.review["reviewed"]["path"]
        self.review_path = self.root / "current-review.json"
        self.write_json(self.review_path, self.review)

    def tearDown(self) -> None:
        # A current-candidate review never overwrites the accepted historical artifact.
        self.assertEqual(self.original_review,
                         (self.root / "i18n/quality/reviews/fr-FR.json").read_bytes())

    def write_json(self, path: Path, value: object) -> None:
        path.write_bytes(extraction.json_bytes(value))

    def reconcile(self) -> dict:
        return verify.reconcile(self.root, self.manifest_path,
                                self.root / "i18n/locales.json",
                                self.root / "i18n/state/translations.json")

    def synchronize(self) -> tuple[dict, dict]:
        self.manifest["messages"].sort(key=lambda entry: entry["id"])
        self.write_json(self.manifest_path, self.manifest)
        report = {"$schema": "fixture", "schema_version": 1,
                  "source_manifest": "i18n/extracted/messages.json", "locales": {}}
        for path in sorted((self.root / "i18n/app").glob("*.ts")):
            tree = extraction.parse_catalog(path)
            tag = extraction.locale_tag(tree, path)
            content, status, _ = extraction.synchronize_catalog(path, tree, self.manifest, "en-US")
            path.write_bytes(content)
            report["locales"][tag] = {"catalog": path.relative_to(self.root).as_posix(), **status}
        return self.reconcile(), report

    def workset(self, state: dict) -> dict:
        current = {entry["id"]: entry for entry in self.manifest["messages"]}
        payloads = {
            tag: verify.read_catalog(self.root / locale["catalog"], tag, current)
            for tag, locale in state["locales"].items() if locale["enabled"]
        }
        return check_diff.build_workset(
            base_label="accepted-fixture", source_manifest="i18n/extracted/messages.json",
            base_messages=self.base, current_messages=current, registry=self.registry,
            state=state, payloads=payloads, policy_root=ROOT)

    def assert_unchanged_trust(self, state: dict, excluded: set[str]) -> None:
        for tag, locale in state["locales"].items():
            if not locale["enabled"]:
                continue
            for message_id in set(self.base) - excluded:
                self.assertEqual(self.prior_state["locales"][tag]["messages"][message_id],
                                 locale["messages"][message_id], f"{tag}/{message_id}")

    def refresh_current_receipt(self) -> None:
        # Fixture-only receipt updates; preserve every existing per-ID review decision.
        self.review["reviewed"]["sha256"] = linguistic_qa.sha256(self.catalog_path)
        self.review["reviewed"]["message_count"] = len(self.manifest["messages"])
        for surface in self.review["surfaces"]:
            if surface["surface"] == "application":
                surface["sha256"] = linguistic_qa.sha256(self.catalog_path)
                surface["reviewed_units"] = len(self.manifest["messages"])
        self.write_json(self.review_path, self.review)

    def test_unchanged_corpus_preserves_all_reviewed_state_and_needs_no_work(self) -> None:
        state, _ = self.synchronize()
        self.assertEqual(self.prior_state, state)
        self.assertEqual([], verify.release_failures(state))
        self.assert_unchanged_trust(state, set())
        workset = self.workset(state)
        self.assertEqual([], workset["delta"]["added"])
        self.assertEqual([], workset["delta"]["changed"])
        for locale in workset["locales"].values():
            self.assertEqual([], locale["required"])
            self.assertIsNone(locale["queue"])
        linguistic_qa.validate_review(self.root, self.review_path)

    def test_one_added_id_requires_only_new_work_and_expanded_review(self) -> None:
        message = copy.deepcopy(next(entry for entry in self.base.values() if not entry["plural"]))
        message["id"] = "gamehq.fixture.new_review_unit"
        self.manifest["messages"].append(message)
        state, report = self.synchronize()
        self.assert_unchanged_trust(state, set())
        workset = self.workset(state)
        self.assertEqual([message["id"]], workset["delta"]["added"])
        for tag, locale in workset["locales"].items():
            self.assertEqual([message["id"]], locale["required"])
            self.assertEqual([message["id"]], [unit["id"] for unit in locale["queue"]["units"]])
            self.assertEqual([message["id"]], report["locales"][tag]["missing"])
        self.assertTrue(verify.release_failures(state))
        with self.assertRaisesRegex(linguistic_qa.ReviewError, "unfinished catalog message"):
            linguistic_qa.validate_review(self.root, self.review_path)

        # Supply a reviewed translation only in this isolated test fixture.
        tree = ET.parse(self.catalog_path).getroot()
        translation = tree.find(f".//message[@id='{message['id']}']/translation")
        translation.attrib.pop("type")
        translation.text = "Traduction de test"
        self.catalog_path.write_bytes(extraction.catalog_bytes(tree))
        self.refresh_current_receipt()
        with self.assertRaisesRegex(linguistic_qa.ReviewError, "coverage must contain exactly"):
            linguistic_qa.validate_review(self.root, self.review_path)
        self.review["coverage"].append({
            "id": message["id"], "source_hash": message["source_hash"],
            "translation_hash": linguistic_qa.translation_hash([translation.text]),
            "locations": [f"{location['file']}:{location['line']}" for location in message["locations"]],
            "decision": "accepted",
        })
        self.refresh_current_receipt()
        linguistic_qa.validate_review(self.root, self.review_path)
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/i18n/linguistic_qa.py"),
             "--root", str(self.root), "--artifact", str(self.review_path)],
            capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stderr)
        count = len(self.base) + 1
        self.assertIn(f"{count}/{count} IDs", result.stdout)

    def test_one_changed_source_invalidates_only_its_review(self) -> None:
        message = self.manifest["messages"][0]
        message_id = message["id"]
        message["source"] += " now"
        message["source_hash"] = extraction.message_source_hash(message["source"], message["context"])
        state, report = self.synchronize()
        self.assert_unchanged_trust(state, {message_id})
        workset = self.workset(state)
        self.assertEqual([message_id], workset["delta"]["changed"])
        for tag, locale in workset["locales"].items():
            self.assertEqual([message_id], locale["required"])
            self.assertEqual("stale", state["locales"][tag]["messages"][message_id]["status"])
            self.assertEqual([message_id], report["locales"][tag]["stale"])
        self.assertTrue(verify.release_failures(state))
        # Even clearing Qt's unfinished flag cannot make the old source review current.
        tree = ET.parse(self.catalog_path).getroot()
        tree.find(f".//message[@id='{message_id}']/translation").attrib.pop("type")
        self.catalog_path.write_bytes(extraction.catalog_bytes(tree))
        self.refresh_current_receipt()
        with self.assertRaisesRegex(linguistic_qa.ReviewError, "stale reviewed source hash"):
            linguistic_qa.validate_review(self.root, self.review_path)

    def test_one_removed_id_retains_history_and_accepts_smaller_current_review(self) -> None:
        removed = self.manifest["messages"].pop(0)["id"]
        old_forms = linguistic_qa.catalog_messages(self.catalog_path)[removed]
        state, report = self.synchronize()
        self.assert_unchanged_trust(state, {removed})
        self.assertEqual([], verify.release_failures(state))
        workset = self.workset(state)
        self.assertEqual([removed], workset["delta"]["removed"])
        check_diff.validate_obsolete_history(report, [removed])
        for tag, locale in workset["locales"].items():
            self.assertNotIn(removed, state["locales"][tag]["messages"])
            self.assertEqual([], locale["required"])
        tree = ET.parse(self.catalog_path).getroot()
        translation = tree.find(f".//message[@id='{removed}']/translation")
        self.assertEqual("vanished", translation.get("type"))
        self.assertEqual(old_forms, ["".join(translation.itertext())])
        self.refresh_current_receipt()
        with self.assertRaisesRegex(linguistic_qa.ReviewError, "coverage must contain exactly"):
            linguistic_qa.validate_review(self.root, self.review_path)
        self.review["coverage"] = [entry for entry in self.review["coverage"] if entry["id"] != removed]
        self.review["corrections"] = [entry for entry in self.review["corrections"]
                                      if entry["surface"] != "application" or entry["id"] != removed]
        self.refresh_current_receipt()
        linguistic_qa.validate_review(self.root, self.review_path)
        translation.set("type", "obsolete")
        self.catalog_path.write_bytes(extraction.catalog_bytes(tree))
        self.refresh_current_receipt()
        linguistic_qa.validate_review(self.root, self.review_path)

    def test_incomplete_enabled_locale_fails_closed(self) -> None:
        message_id = next(iter(self.base))
        tree = ET.parse(self.catalog_path).getroot()
        tree.find(f".//message[@id='{message_id}']/translation").set("type", "unfinished")
        self.catalog_path.write_bytes(extraction.catalog_bytes(tree))
        state = self.reconcile()
        failures = verify.release_failures(state)
        self.assertEqual(1, len(failures))
        self.assertIn("fr-FR/application is incomplete", failures[0])
        self.assert_unchanged_trust(state, {message_id})
        self.refresh_current_receipt()
        with self.assertRaisesRegex(linguistic_qa.ReviewError, "unfinished catalog message"):
            linguistic_qa.validate_review(self.root, self.review_path)

    def test_empty_duplicate_and_malformed_manifests_fail_closed(self) -> None:
        original = copy.deepcopy(self.manifest)
        for messages, diagnostic in (
            ([], "must not be empty"),
            ([original["messages"][0]] * 2, "not uniquely sorted"),
            ([{"id": "gamehq.fixture.invalid"}], "does not match its schema"),
        ):
            with self.subTest(diagnostic=diagnostic):
                self.manifest["messages"] = messages
                self.write_json(self.manifest_path, self.manifest)
                with self.assertRaisesRegex(linguistic_qa.ReviewError, diagnostic):
                    linguistic_qa.validate_review(self.root, self.review_path)

    def test_duplicate_active_catalog_id_fails_closed(self) -> None:
        tree = ET.parse(self.catalog_path).getroot()
        context = tree.find("context")
        context.append(copy.deepcopy(context.find("message")))
        self.catalog_path.write_bytes(extraction.catalog_bytes(tree))
        self.refresh_current_receipt()
        with self.assertRaisesRegex(linguistic_qa.ReviewError, "duplicate catalog ID"):
            linguistic_qa.validate_review(self.root, self.review_path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
