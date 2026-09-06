#!/usr/bin/env python3
"""Focused tests for localization release-readiness governance."""

from __future__ import annotations

import copy
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools" / "i18n"
sys.path.insert(0, str(TOOLS))

import release_readiness as readiness  # noqa: E402


def load_generator():
    """Load the release-note generator to pin the duplicated integrity algorithm."""
    path = TOOLS / "generate_release_notes.py"
    spec = importlib.util.spec_from_file_location("gamehq_release_notes_readiness", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

CANDIDATE_VERSION = "0.7.7"
FINAL_DATE = "2026-09-30"
READINESS_INPUTS = (
    "VERSION",
    "src/ui/qml/Brand.qml",
    "packaging/i18n/custom-messages.json",
    "i18n/locales.json",
    "i18n/extracted/messages.json",
    "i18n/state/translations.json",
    "i18n/app",
    "i18n/style",
    "i18n/release",
    "i18n/quality/reviews",
    "integrations/playnite/src/GameHQ.Playnite/Localization",
    "assets/release-notes",
)
# Assigning the release date rewrites every localized release-note document, so a
# real finalization also has to re-pin the release-note surface hash each locale
# review records. The fixture reproduces that instead of hiding it.
NOTE_SURFACE = "release_notes"


def copy_readiness_inputs(source: Path, destination: Path) -> None:
    for relative in READINESS_INPUTS:
        origin = source / relative
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if origin.is_dir():
            shutil.copytree(origin, target)
        else:
            shutil.copy2(origin, target)


def write_json(path: Path, value: object) -> None:
    path.write_bytes(readiness.json_bytes(value))


def tree_digest(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): readiness.sha256_bytes(path.read_bytes())
        for path in sorted(root.rglob("*")) if path.is_file()
    }


def finalize_repository(
    root: Path, *, version: str = CANDIDATE_VERSION, date: str | None = FINAL_DATE,
    repository_version: str | None = None, status: str = "released",
    promote: bool = True, policy: str = "complete", entry_date: str | None = None,
    entry_status: str = "released", integrity: str | None = None,
    original_integrity: str | None = None, correction: object = None,
    duplicate: bool = False, reorder: bool = False, stamp_documents: bool = True,
) -> None:
    """Rewrite a copied repository into an explicitly finalized release state."""
    (root / "VERSION").write_text(f"{repository_version or version}\n", encoding="utf-8")
    if stamp_documents:
        for path in sorted((root / f"assets/release-notes/versions/{version}").glob("*.json")):
            document = readiness.read_json(path)
            document["date"] = date
            write_json(path, document)
        for path in sorted((root / "i18n/quality/reviews").glob("*.json")):
            review = readiness.read_json(path)
            for surface in review["surfaces"]:
                if surface["surface"] == NOTE_SURFACE:
                    surface["sha256"] = readiness.sha256_bytes((root / surface["path"]).read_bytes())
            write_json(path, review)
    manifest = readiness.read_json(root / "assets/release-notes/manifest.json")
    manifest["localization_launch"] |= {"version": version, "date": date, "status": status}
    if promote:
        english = readiness.read_json(
            root / f"assets/release-notes/versions/{version}/en-US.json")
        canonical = readiness.release_note_integrity(english)
        entry = {
            "version": version, "date": entry_date or date, "status": entry_status,
            "localization_policy": policy, "source_integrity": integrity or canonical,
            "original_source_integrity": original_integrity or integrity or canonical,
            "correction": correction,
        }
        manifest["releases"].insert(len(manifest["releases"]) if reorder else 0, entry)
        if duplicate:
            manifest["releases"].insert(1, copy.deepcopy(entry))
    write_json(root / "assets/release-notes/manifest.json", manifest)


class ReleaseReadinessTest(unittest.TestCase):
    def setUp(self) -> None:
        self.evidence = readiness.build_evidence(ROOT)

    def test_committed_snapshot_is_byte_current_and_deterministic(self) -> None:
        path = ROOT / "i18n/release/readiness-0.7.7.json"
        first = readiness.json_bytes(self.evidence)
        second = readiness.json_bytes(readiness.build_evidence(ROOT))
        self.assertEqual(first, second)
        self.assertEqual(first, path.read_bytes())

    def test_portfolio_order_and_release_authority_stay_separate(self) -> None:
        portfolio = self.evidence["portfolio"]
        self.assertEqual(16, len(portfolio["production_locales"]))
        self.assertEqual(["en-XA", "ar-XB"], portfolio["development_only"])
        self.assertEqual(["cs-CZ"], portfolio["reserve"])
        self.assertEqual(list(range(1, 9)),
                         [step["order"] for step in self.evidence["workflow"]])
        self.assertEqual("p8-3 only", self.evidence["workflow"][6]["gate"])
        self.assertEqual("p8-4 only", self.evidence["workflow"][7]["gate"])
        self.assertTrue(all(item["state"] == "pending" for item in self.evidence["handoff"]))
        self.assertFalse(
            self.evidence["provenance_contract"]
            ["machine_output_self_certifies_linguistic_quality"]
        )
        self.assertIsNone(self.evidence["candidate"]["date"])
        self.assertEqual("not_requested", self.evidence["candidate"]["release_authorization"])
        self.assertEqual("prohibited", self.evidence["candidate"]["publication_state"])

    def test_every_locale_carries_sidebar_and_truthful_qa_evidence(self) -> None:
        expected_ids = list(readiness.REQUIRED_SURFACES)
        self.assertEqual(16, len(self.evidence["locales"]))
        for locale, value in self.evidence["locales"].items():
            self.assertEqual(expected_ids, [entry["id"] for entry in value["required_surfaces"]], locale)
            self.assertTrue(all(entry["catalog_translation_hash"]
                                for entry in value["required_surfaces"]), locale)
            review_path = ROOT / f"i18n/quality/reviews/{locale}.json"
            if review_path.is_file():
                self.assertEqual("contextually_reviewed", value["linguistic_qa"]["state"])
                self.assertEqual(f"i18n/quality/reviews/{locale}.json",
                                 value["linguistic_qa"]["artifact"]["path"])
            else:
                self.assertEqual(
                    {"state": "pending", "authority": "p8-3", "artifact": None},
                    value["linguistic_qa"], locale,
                )

    def test_correction_ledger_rejects_unscoped_private_or_false_acceptance(self) -> None:
        corrections = readiness.read_json(ROOT / "i18n/release/corrections.json")
        tags = list(self.evidence["portfolio"]["production_locales"])
        extracted = readiness.read_json(ROOT / "i18n/extracted/messages.json")
        locales = self.evidence["locales"]
        readiness.validate_corrections(corrections, ROOT, tags, extracted, locales)
        entry = corrections["entries"][0]
        self.assertEqual("pl-PL", entry["locale"])
        self.assertEqual("gamehq.navigation.about", entry["message_id"])
        self.assertEqual("contextually_reviewed", entry["review_state"])

        mutations = []
        unsupported = copy.deepcopy(corrections)
        unsupported["entries"][0]["locale"] = "cs-CZ"
        mutations.append((unsupported, "unsupported production locale"))
        stale = copy.deepcopy(corrections)
        stale["entries"][0]["source_hash"] = "0" * 64
        mutations.append((stale, "stale source hash"))
        private = copy.deepcopy(corrections)
        private["entries"][0]["telemetry"] = "forbidden"
        mutations.append((private, "forbidden private field"))
        false_acceptance = copy.deepcopy(corrections)
        false_acceptance["entries"][0]["review_state"] = "linguistically_accepted"
        mutations.append((false_acceptance, "only human contextual review"))
        false_context = copy.deepcopy(corrections)
        false_context["entries"][0]["method"] = "machine_verification"
        mutations.append((false_context, "explicit method"))
        for document, diagnostic in mutations:
            with self.subTest(diagnostic=diagnostic):
                with self.assertRaisesRegex(readiness.ReadinessError, diagnostic):
                    readiness.validate_corrections(document, ROOT, tags, extracted, locales)

    def test_check_mode_rejects_stale_output_without_rewriting_it(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gamehq-readiness-") as temporary:
            output = Path(temporary) / "readiness.json"
            generated = subprocess.run(
                [sys.executable, str(TOOLS / "release_readiness.py"),
                 "--root", str(ROOT), "--output", str(output)],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            self.assertEqual(0, generated.returncode, generated.stdout + generated.stderr)
            output.write_bytes(output.read_bytes() + b"\n")
            stale = output.read_bytes()
            checked = subprocess.run(
                [sys.executable, str(TOOLS / "release_readiness.py"),
                 "--root", str(ROOT), "--output", str(output), "--check"],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            self.assertNotEqual(0, checked.returncode)
            self.assertIn("stale release-readiness evidence", checked.stdout + checked.stderr)
            self.assertEqual(stale, output.read_bytes())

    def test_fast_ci_requires_readiness_evidence_and_focused_fixtures(self) -> None:
        gate = (TOOLS / "ci.ps1").read_text(encoding="utf-8")
        self.assertIn("release_readiness.py", gate)
        self.assertIn("test_release_readiness.py", gate)
        for forbidden in ("Invoke-WebRequest", "Invoke-RestMethod", "curl ", "secrets."):
            self.assertNotIn(forbidden.casefold(), gate.casefold())


class ReleaseStateModeTest(unittest.TestCase):
    """Candidate mode protects the pre-release state; final mode validates a finalized one."""

    MUTABLE = ("VERSION", "assets/release-notes/manifest.json")

    @classmethod
    def setUpClass(cls) -> None:
        cls._temporary = tempfile.TemporaryDirectory(prefix="gamehq-release-state-")
        cls.workspace = Path(cls._temporary.name)
        cls.root = cls.workspace / "repository"
        copy_readiness_inputs(ROOT, cls.root)

    @classmethod
    def tearDownClass(cls) -> None:
        cls._temporary.cleanup()

    def setUp(self) -> None:
        for relative in self.MUTABLE:
            shutil.copy2(ROOT / relative, self.root / relative)
        for directory in (f"assets/release-notes/versions/{CANDIDATE_VERSION}",
                          "i18n/quality/reviews"):
            for path in sorted((ROOT / directory).glob("*.json")):
                shutil.copy2(path, self.root / directory / path.name)

    def run_tool(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOLS / "release_readiness.py"), "--root", str(self.root),
             *arguments],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )

    def test_copied_candidate_repository_passes_candidate_mode_only(self) -> None:
        evidence = readiness.build_evidence(self.root)
        self.assertEqual("not_requested", evidence["candidate"]["release_authorization"])
        self.assertNotIn("mode", evidence)
        # Final-state validation must never change what the candidate gate emits.
        self.assertEqual((ROOT / "i18n/release/readiness-0.7.7.json").read_bytes(),
                         readiness.json_bytes(evidence))
        with self.assertRaisesRegex(readiness.ReadinessError,
                                    "final mode requires a released localization launch"):
            readiness.build_evidence(self.root, readiness.FINAL_MODE)

    def test_coherent_finalized_repository_passes_final_mode_only(self) -> None:
        finalize_repository(self.root)
        evidence = readiness.build_evidence(self.root, readiness.FINAL_MODE)
        self.assertEqual("final", evidence["mode"])
        self.assertNotIn("candidate", evidence)
        release = evidence["release"]
        self.assertEqual(
            {"version": CANDIDATE_VERSION, "date": FINAL_DATE, "designation": "owner",
             "repository_version": CANDIDATE_VERSION},
            {key: release[key] for key in
             ("version", "date", "designation", "repository_version")},
        )
        self.assertEqual("not_validated", release["release_authorization"])
        self.assertEqual("requires_owner_authorization", release["publication_state"])
        self.assertEqual(CANDIDATE_VERSION, release["release_entry"]["version"])
        self.assertEqual("complete", release["release_entry"]["localization_policy"])
        self.assertEqual(16, len(evidence["locales"]))
        with self.assertRaisesRegex(readiness.ReadinessError,
                                    "owner-designated and date-null"):
            readiness.build_evidence(self.root, readiness.CANDIDATE_MODE)

    def test_readiness_and_generator_compute_the_same_release_integrity(self) -> None:
        """The duplicated integrity computation must never drift from the generator."""
        generator = load_generator()
        for version in ("0.7.6", CANDIDATE_VERSION):
            document = readiness.read_json(
                ROOT / f"assets/release-notes/versions/{version}/en-US.json")
            with self.subTest(version=version):
                self.assertEqual(generator.source_integrity(document),
                                 readiness.release_note_integrity(document))
                self.assertEqual(generator.source_integrity({**document, "date": FINAL_DATE}),
                                 readiness.release_note_integrity({**document,
                                                                   "date": FINAL_DATE}))

    def test_partially_finalized_repositories_fail_final_mode(self) -> None:
        mutations = (
            ("repository VERSION still trails the release",
             {"repository_version": "0.7.6"}, "repository VERSION 0.7.6 differs"),
            ("the launch was never flipped to released",
             {"status": "designated"}, "final mode requires a released localization launch"),
            ("the launch carries no release date",
             {"date": None}, "expected an ISO-8601 calendar date"),
            ("the release entry contradicts the launch date",
             {"entry_date": "2026-10-01"}, "instead of the launch date"),
            ("the release entry allows English fallback",
             {"policy": "fallback-allowed"}, "must require all sixteen locales"),
            ("the release history records the version twice",
             {"duplicate": True}, "records a version more than once"),
            ("the release history is not newest-first",
             {"version": "0.7.7", "date": FINAL_DATE, "reorder": True},
             "not deterministically newest-first"),
            ("the localized release notes keep a null date",
             {"stamp_documents": False}, "instead of the release date"),
            ("the release was never promoted into the history",
             {"promote": False}, "was never promoted into the release history"),
            ("the release history entry is not marked released",
             {"entry_status": "designated"}, "is not marked released"),
            ("the recorded source integrity does not match the English document",
             {"integrity": f"sha256:{'0' * 64}"}, "source_integrity does not match"),
            ("the entry claims a corrected original source",
             {"original_integrity": f"sha256:{'1' * 64}"},
             "original_source_integrity does not match"),
            ("the first publication already claims a correction",
             {"correction": {"reason": "typo", "approved_by": "owner",
                             "corrected_at": FINAL_DATE}},
             "records a correction before it was ever released"),
        )
        for diagnostic, overrides, expected in mutations:
            with self.subTest(diagnostic=diagnostic):
                self.setUp()
                finalize_repository(self.root, **overrides)
                with self.assertRaisesRegex(readiness.ReadinessError, expected):
                    readiness.build_evidence(self.root, readiness.FINAL_MODE)

    def test_final_mode_requires_an_explicit_output_and_never_writes_the_repository(self) -> None:
        finalize_repository(self.root)
        before = tree_digest(self.root)
        missing = self.run_tool("--mode", "final", "--check")
        self.assertNotEqual(0, missing.returncode)
        self.assertIn("final mode requires an explicit --output path",
                      missing.stdout + missing.stderr)

        output = self.workspace / "final-readiness.json"
        generated = self.run_tool("--mode", "final", "--output", str(output))
        self.assertEqual(0, generated.returncode, generated.stdout + generated.stderr)
        self.assertIn("publication still requires explicit owner authorization",
                      generated.stdout)
        payload = output.read_bytes()
        checked = self.run_tool("--mode", "final", "--output", str(output), "--check")
        self.assertEqual(0, checked.returncode, checked.stdout + checked.stderr)
        self.assertEqual(payload, output.read_bytes())

        output.write_bytes(payload + b"\n")
        stale = self.run_tool("--mode", "final", "--output", str(output), "--check")
        self.assertNotEqual(0, stale.returncode)
        self.assertEqual(payload + b"\n", output.read_bytes())
        self.assertEqual(before, tree_digest(self.root))

    def test_default_invocation_stays_candidate_and_leaves_the_checkout_alone(self) -> None:
        before = tree_digest(self.root)
        checked = self.run_tool("--check")
        self.assertEqual(0, checked.returncode, checked.stdout + checked.stderr)
        self.assertIn("publication prohibited", checked.stdout)
        self.assertEqual(before, tree_digest(self.root))
        gate = (TOOLS / "ci.ps1").read_text(encoding="utf-8")
        self.assertNotIn("--mode", gate)
        self.assertNotIn("final", gate)
        workflow = (ROOT / ".github/workflows/unsigned-beta.yml").read_text(encoding="utf-8")
        self.assertNotIn("release_readiness.py", workflow)


class ReadinessSchemaTest(unittest.TestCase):
    """The schema and the generator must describe the same two document shapes."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.schema = readiness.read_json(ROOT / "i18n/schema/release-readiness.schema.json")
        cls._temporary = tempfile.TemporaryDirectory(prefix="gamehq-readiness-schema-")
        cls.root = Path(cls._temporary.name) / "repository"
        copy_readiness_inputs(ROOT, cls.root)
        cls.candidate = readiness.build_evidence(cls.root)
        finalize_repository(cls.root)
        cls.final = readiness.build_evidence(cls.root, readiness.FINAL_MODE)

    @classmethod
    def tearDownClass(cls) -> None:
        cls._temporary.cleanup()

    def definition(self, name: str) -> dict[str, object]:
        return self.schema["definitions"][name]

    def test_declared_document_shapes_match_the_generated_evidence(self) -> None:
        self.assertEqual(
            [{"$ref": "#/definitions/candidateDocument"},
             {"$ref": "#/definitions/finalDocument"}],
            self.schema["oneOf"],
        )
        for name, evidence in (("candidateDocument", self.candidate),
                               ("finalDocument", self.final)):
            with self.subTest(document=name):
                declared = self.definition(name)
                self.assertFalse(declared["additionalProperties"])
                self.assertEqual(sorted(evidence), sorted(declared["required"]))
                self.assertEqual(sorted(evidence), sorted(declared["properties"]))

    def test_declared_release_state_matches_the_implementation(self) -> None:
        candidate = self.definition("candidateDocument")["properties"]["candidate"]
        self.assertEqual(sorted(self.candidate["candidate"]), sorted(candidate["required"]))
        self.assertEqual("not_requested",
                         candidate["properties"]["release_authorization"]["const"])
        self.assertEqual("prohibited", candidate["properties"]["publication_state"]["const"])
        self.assertEqual({"type": "null"}, candidate["properties"]["date"])

        release = self.definition("finalDocument")["properties"]["release"]
        self.assertEqual(sorted(self.final["release"]), sorted(release["required"]))
        self.assertEqual("not_validated",
                         release["properties"]["release_authorization"]["const"])
        self.assertEqual("requires_owner_authorization",
                         release["properties"]["publication_state"]["const"])
        self.assertEqual(
            sorted(readiness.RELEASE_ENTRY_FIELDS),
            sorted(self.definition("releaseEntry")["required"]),
        )
        self.assertEqual(
            "contextually_reviewed",
            self.definition("reviewedLocale")["allOf"][1]["properties"]["linguistic_qa"]
            ["properties"]["state"]["const"],
        )

    def test_both_documents_validate_against_the_published_schema(self) -> None:
        try:
            import jsonschema
        except ImportError:  # pragma: no cover - offline CI images may omit the dependency
            self.skipTest("jsonschema is unavailable")
        jsonschema.Draft7Validator.check_schema(self.schema)
        validator = jsonschema.Draft7Validator(self.schema)
        for label, evidence in (("candidate", self.candidate), ("final", self.final)):
            with self.subTest(document=label):
                self.assertEqual([], [error.message for error in validator.iter_errors(evidence)])
        committed = readiness.read_json(ROOT / "i18n/release/readiness-0.7.7.json")
        self.assertEqual([], [error.message for error in validator.iter_errors(committed)])
        broken = copy.deepcopy(self.final)
        broken["release"]["release_authorization"] = "owner_authorized"
        self.assertTrue(list(validator.iter_errors(broken)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
