#!/usr/bin/env python3
"""Focused validation for the one-command locale lifecycle (p7-3).

Every mutating case runs against an isolated temporary root with a disabled
fixture locale. `cs-CZ` is never promoted and the sixteen production locales are
never touched.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import locale_lifecycle as lifecycle


ROOT = lifecycle.ROOT
FIXTURE = "fr-CA"
FIXTURE_ENGLISH = "French (Canada)"
FIXTURE_NATIVE = "Francais (Canada)"
CODE_BLOCK = re.compile(r"```powershell\n(.*?)```", re.DOTALL)
GUIDE = ROOT / "docs" / "localization" / "CONTRIBUTING.md"


def add_args(tag: str = FIXTURE, **overrides) -> argparse.Namespace:
    values = {
        "tag": tag,
        "english_name": FIXTURE_ENGLISH,
        "native_name": FIXTURE_NATIVE,
        "direction": "ltr",
        "fallback": "en-US",
        "alias": [],
        "check": False,
    }
    values.update(overrides)
    return argparse.Namespace(command="add", **values)


def run(command, root: Path, args: argparse.Namespace) -> str:
    stream = io.StringIO()
    with contextlib.redirect_stdout(stream):
        command(root, args)
    return stream.getvalue()


def tree_bytes(root: Path) -> dict[str, bytes]:
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in sorted(root.rglob("*")) if path.is_file()
    }


def registry_of(root: Path) -> dict:
    return json.loads((root / "i18n" / "locales.json").read_text(encoding="utf-8"))


def production_entries(root: Path) -> dict[str, dict]:
    return {entry["tag"]: entry for entry in registry_of(root)["locales"]
            if entry.get("tier") == 1 and entry.get("state") == "enabled"}


class LocaleLifecycleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        (self.root).mkdir()
        shutil.copytree(ROOT / "i18n", self.root / "i18n")

    def with_fixture_catalog(self) -> None:
        """Give the fixture locale real translated content and provenance."""
        source = (self.root / "i18n/app/gamehq_fr_FR.ts").read_text(encoding="utf-8")
        (self.root / "i18n/app/gamehq_fr_CA.ts").write_text(
            source.replace('language="fr_FR"', 'language="fr_CA"'),
            encoding="utf-8", newline="\n")

    # ------------------------------------------------------------------ add

    def test_onboarding_is_deterministic_and_idempotent(self) -> None:
        first = run(lifecycle.command_add, self.root, add_args())
        snapshot = tree_bytes(self.root)
        second = run(lifecycle.command_add, self.root, add_args())
        self.assertEqual(snapshot, tree_bytes(self.root))
        self.assertIn("register fr-CA", first)
        self.assertIn("already registered", second)

        other = Path(self.temp.name) / "repo2"
        shutil.copytree(ROOT / "i18n", other / "i18n")
        run(lifecycle.command_add, other, add_args())
        self.assertEqual(tree_bytes(self.root), tree_bytes(other))

    def test_check_mode_is_non_mutating(self) -> None:
        before = tree_bytes(self.root)
        output = run(lifecycle.command_add, self.root, add_args(check=True))
        self.assertEqual(before, tree_bytes(self.root))
        self.assertIn("would register", output)
        self.assertIn("dry run: no file was written", output)
        for verb, command in (("disable", lifecycle.command_disable),
                              ("retire", lifecycle.command_retire),
                              ("restore", lifecycle.command_restore)):
            run(lifecycle.command_add, self.root, add_args())
            snapshot = tree_bytes(self.root)
            run(command, self.root,
                argparse.Namespace(command=verb, tag=FIXTURE, check=True))
            self.assertEqual(snapshot, tree_bytes(self.root), verb)

    def test_missing_required_metadata_fails_clearly(self) -> None:
        for missing in ("english_name", "native_name"):
            with self.assertRaises(lifecycle.LocaleError) as caught:
                lifecycle.plan_add(self.root, add_args(**{missing: None}))
            self.assertIn("--english-name and --native-name", str(caught.exception))
        with self.assertRaises(lifecycle.LocaleError) as caught:
            lifecycle.plan_add(self.root, add_args(tag="Not_A_Tag"))
        self.assertIn("canonical BCP-47", str(caught.exception))
        with self.assertRaises(lifecycle.LocaleError) as caught:
            lifecycle.plan_add(self.root, add_args(fallback="qq-QQ"))
        self.assertIn("not registered", str(caught.exception))

    def test_duplicate_locale_and_alias_collisions_fail(self) -> None:
        with self.assertRaises(lifecycle.LocaleError) as caught:
            lifecycle.plan_add(self.root, add_args(tag="pl-PL"))
        self.assertIn("enabled production locale", str(caught.exception))
        with self.assertRaises(lifecycle.LocaleError) as caught:
            lifecycle.plan_add(self.root, add_args(tag="pl", alias=[]))
        self.assertIn("already an alias", str(caught.exception))
        with self.assertRaises(lifecycle.LocaleError) as caught:
            lifecycle.plan_add(self.root, add_args(alias=["pl-PL"]))
        self.assertIn("collides with the existing locale", str(caught.exception))
        with self.assertRaises(lifecycle.LocaleError) as caught:
            lifecycle.plan_add(self.root, add_args(alias=["pl"]))
        self.assertIn("already mapped to", str(caught.exception))
        with self.assertRaises(lifecycle.LocaleError) as caught:
            lifecycle.plan_add(self.root, add_args(alias=["fr-QC", "fr-QC"]))
        self.assertIn("duplicate alias", str(caught.exception))

    def test_new_locale_is_disabled_and_not_release_eligible(self) -> None:
        run(lifecycle.command_add, self.root, add_args(alias=["fr-QC"]))
        entry = {e["tag"]: e for e in registry_of(self.root)["locales"]}[FIXTURE]
        self.assertEqual(entry["state"], "reserve")
        self.assertEqual(entry["tier"], 0)
        self.assertIsNone(entry["inno_language"])
        self.assertIsNone(entry["inno_message_file"])
        self.assertEqual(entry["completeness_policy"], "complete_at_release")
        self.assertEqual(entry["qt_catalog"], "gamehq_fr_CA")
        self.assertEqual(registry_of(self.root)["aliases"]["fr-QC"], FIXTURE)
        state = json.loads((self.root / "i18n/state/translations.json").read_text(encoding="utf-8"))
        self.assertFalse(state["locales"][FIXTURE]["catalog_present"])
        self.assertFalse(state["locales"][FIXTURE]["enabled"])
        # The scaffolded style guide is explicitly unreviewed.
        self.assertFalse(lifecycle.style_is_authored(self.root, FIXTURE))
        style = json.loads(
            (self.root / f"i18n/style/{FIXTURE}.json").read_text(encoding="utf-8"))
        self.assertTrue(all(style[field].startswith(lifecycle.UNREVIEWED)
                            for field in lifecycle.STYLE_FIELDS))

    def test_production_locales_are_unchanged_by_onboarding(self) -> None:
        before = production_entries(self.root)
        before_files = tree_bytes(self.root)
        run(lifecycle.command_add, self.root, add_args(alias=["fr-QC"]))
        after = production_entries(self.root)
        self.assertEqual(len(after), lifecycle.PRODUCTION_COUNT)
        self.assertEqual(before, after)
        after_files = tree_bytes(self.root)
        changed = {name for name in before_files if before_files[name] != after_files.get(name)}
        self.assertEqual(changed, {"i18n/locales.json", "i18n/state/translations.json"})
        # cs-CZ stays the untouched first reserve locale.
        entries = {e["tag"]: e for e in registry_of(self.root)["locales"]}
        self.assertEqual(entries[lifecycle.FIRST_RESERVE]["state"], "reserve")
        before_state = json.loads(before_files["i18n/state/translations.json"])
        after_state = json.loads(after_files["i18n/state/translations.json"])
        self.assertEqual(set(after_state["locales"]) - set(before_state["locales"]), {FIXTURE})
        for tag in before_state["locales"]:
            self.assertEqual(before_state["locales"][tag], after_state["locales"][tag], tag)

    def test_pseudo_locales_can_never_be_registered_or_promoted(self) -> None:
        for tag in lifecycle.PSEUDO_LOCALES:
            with self.assertRaises(lifecycle.LocaleError) as caught:
                lifecycle.plan_add(self.root, add_args(tag=tag))
            self.assertIn("pseudo-locale", str(caught.exception))
        registry = registry_of(self.root)
        registry["locales"].append({"tag": "en-XA", "tier": 0, "state": "internal",
                                    "names": {"native": "x", "english": "x"}})
        with self.assertRaises(lifecycle.LocaleError):
            lifecycle.guard_portfolio(registry)

    # ------------------------------------------------ disable / retire / restore

    def test_disable_and_retire_preserve_reviewed_history(self) -> None:
        self.with_fixture_catalog()
        run(lifecycle.command_add, self.root, add_args())
        catalog = (self.root / "i18n/app/gamehq_fr_CA.ts").read_bytes()
        style = (self.root / f"i18n/style/{FIXTURE}.json").read_bytes()
        state = json.loads((self.root / "i18n/state/translations.json").read_text(encoding="utf-8"))
        messages = state["locales"][FIXTURE]["messages"]
        self.assertTrue(state["locales"][FIXTURE]["catalog_present"])

        run(lifecycle.command_disable, self.root,
            argparse.Namespace(command="disable", tag=FIXTURE, check=False))
        entry = {e["tag"]: e for e in registry_of(self.root)["locales"]}[FIXTURE]
        self.assertEqual(entry["state"], "disabled")
        self.assertEqual(entry["completeness_policy"], "complete_at_release")

        output = run(lifecycle.command_retire, self.root,
                     argparse.Namespace(command="retire", tag=FIXTURE, check=False))
        entry = {e["tag"]: e for e in registry_of(self.root)["locales"]}[FIXTURE]
        self.assertEqual(entry["state"], "disabled")
        self.assertEqual(entry["completeness_policy"], "never_embed")
        self.assertIn("history are kept", output)

        self.assertEqual(catalog, (self.root / "i18n/app/gamehq_fr_CA.ts").read_bytes())
        self.assertEqual(style, (self.root / f"i18n/style/{FIXTURE}.json").read_bytes())
        after = json.loads(
            (self.root / "i18n/state/translations.json").read_text(encoding="utf-8"))
        self.assertEqual(messages, after["locales"][FIXTURE]["messages"])

        output = run(lifecycle.command_restore, self.root,
                     argparse.Namespace(command="restore", tag=FIXTURE, check=False))
        entry = {e["tag"]: e for e in registry_of(self.root)["locales"]}[FIXTURE]
        self.assertEqual(entry["state"], "reserve")
        self.assertEqual(entry["completeness_policy"], "complete_at_release")
        self.assertIn("never re-enables", output)
        self.assertEqual(messages, after["locales"][FIXTURE]["messages"])

    def test_launch_locales_cannot_be_disabled_or_retired(self) -> None:
        for verb, command in (("disable", lifecycle.command_disable),
                              ("retire", lifecycle.command_retire)):
            with self.assertRaises(lifecycle.LocaleError) as caught:
                run(command, self.root,
                    argparse.Namespace(command=verb, tag="pl-PL", check=False))
            self.assertIn("owner release decision", str(caught.exception))
        self.assertEqual(len(production_entries(self.root)), lifecycle.PRODUCTION_COUNT)

    def test_review_reports_provenance_without_marking_anything_reviewed(self) -> None:
        self.with_fixture_catalog()
        run(lifecycle.command_add, self.root, add_args())
        output = run(lifecycle.command_review, self.root,
                     argparse.Namespace(command="review", tag=FIXTURE))
        self.assertIn("style guide authored: False", output)
        self.assertIn("release-eligible: False", output)
        expected_count = len(lifecycle.read_json(self.root / "i18n/extracted/messages.json")["messages"])
        self.assertIn(f"contextually-reviewed coverage: 0/{expected_count}", output)
        self.assertIn("Generation never marks a message reviewed", output)

    # ----------------------------------------------------------------- verify

    def test_verification_is_deterministic_and_offline(self) -> None:
        first = run(lifecycle.command_verify, ROOT,
                    argparse.Namespace(command="verify"))
        second = run(lifecycle.command_verify, ROOT,
                     argparse.Namespace(command="verify"))
        self.assertEqual(first, second)
        self.assertIn("all localization checks passed offline", first)
        self.assertIn(f"{lifecycle.PRODUCTION_COUNT} production locales", first)
        tool = (ROOT / "tools/i18n/locale_lifecycle.py").read_text(encoding="utf-8")
        for forbidden in ("urllib", "requests", "socket", "http.client", "urlopen"):
            self.assertNotIn(forbidden, tool)

    def test_generated_surface_staleness_is_detected(self) -> None:
        source = Path(self.temp.name) / "release-notes"
        shutil.copytree(ROOT / "assets" / "release-notes", source)
        generated = source / "generated"
        stale = generated / "release-notes.pl-PL.json"
        stale.write_bytes(stale.read_bytes() + b"\n")
        code, message = lifecycle.run_tool(
            [str(lifecycle.TOOLS / "generate_release_notes.py"),
             "--source-root", str(source), "--output-root", str(generated), "--check"],
            "release-note bundles")
        self.assertNotEqual(code, 0)
        self.assertIn("stale", message)
        missing = generated / "release-notes.it-IT.json"
        missing.unlink()
        code, _ = lifecycle.run_tool(
            [str(lifecycle.TOOLS / "generate_release_notes.py"),
             "--source-root", str(source), "--output-root", str(generated), "--check"],
            "release-note bundles")
        self.assertNotEqual(code, 0)

    def test_repository_surface_verbs_reject_a_foreign_root(self) -> None:
        for verb, command, args in (
            ("verify", lifecycle.command_verify, argparse.Namespace(command="verify")),
            ("package", lifecycle.command_package, argparse.Namespace(command="package")),
            ("update", lifecycle.command_update,
             argparse.Namespace(command="update", tag=FIXTURE, check=True)),
        ):
            with self.assertRaises(lifecycle.LocaleError) as caught:
                run(command, self.root, args)
            self.assertIn("cannot run with --root", str(caught.exception))

    def test_package_reports_eligibility_for_every_locale(self) -> None:
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            with contextlib.suppress(lifecycle.LocaleError):
                lifecycle.command_package(ROOT, argparse.Namespace(command="package"))
        output = stream.getvalue()
        for tag in production_entries(ROOT):
            self.assertIn(f"{tag}: packageable", output)
        self.assertIn(f"{lifecycle.FIRST_RESERVE}: not release-eligible", output)

    # ---------------------------------------------------------- documentation

    def test_contributor_guide_examples_are_real(self) -> None:
        text = GUIDE.read_text(encoding="utf-8")
        commands = set(lifecycle.COMMANDS)
        used: set[str] = set()
        for block in CODE_BLOCK.findall(text):
            for line in block.splitlines():
                line = line.strip()
                if not line.startswith((".\\tools\\i18n\\locale.ps1", "python tools/i18n/")):
                    continue
                parts = line.split()
                if "locale.ps1" in parts[0] or parts[1].endswith("locale_lifecycle.py"):
                    verb = parts[1] if "locale.ps1" in parts[0] else parts[2]
                    self.assertIn(verb, commands, line)
                    used.add(verb)
        self.assertEqual(used, commands)
        for path in re.findall(r"`([a-zA-Z0-9_./-]+\.(?:py|ps1|json|rc|md))`", text):
            candidate = ROOT / path
            if "/" in path:
                self.assertTrue(candidate.is_file(), path)
        for tag in production_entries(ROOT):
            self.assertIn(f"`{tag}`", text)
        self.assertIn("`cs-CZ` is the first reserve locale", text)
        self.assertIn("`en-XA` and `ar-XB` are development-only", text)

    def test_powershell_wrapper_exposes_the_same_verbs(self) -> None:
        wrapper = (ROOT / "tools/i18n/locale.ps1").read_text(encoding="utf-8")
        for command in lifecycle.COMMANDS:
            self.assertIn(f"'{command}'", wrapper)
        self.assertIn("locale_lifecycle.py", wrapper)

    def test_command_line_entry_point_runs(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools/i18n/locale_lifecycle.py"), "status"],
            capture_output=True, text=True, encoding="utf-8",
            env={"PYTHONIOENCODING": "utf-8", "PATH": "", "SYSTEMROOT": ""},
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("cs-CZ", completed.stdout)
        self.assertIn("release-eligible=no", completed.stdout)


if __name__ == "__main__":
    unittest.main()
