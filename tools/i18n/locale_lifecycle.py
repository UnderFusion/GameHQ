#!/usr/bin/env python3
"""One command for the GameHQ locale lifecycle.

`locale.py` composes the existing localization tools and canonical contracts.
It never invents parallel metadata, never reaches the network, and never marks
anything reviewed just because generation succeeded.

    add       scaffold a new locale as a disabled reserve locale
    update    refresh the generated surfaces a locale owns
    review    report translation, provenance, and promotion readiness
    disable   stop offering a locale without losing its work
    retire    park a locale so it can never be packaged, keeping its history
    restore   bring a disabled or retired locale back as a reserve locale
    verify    run the deterministic offline localization checks
    package   report release and packaging eligibility per locale
    status    print the current portfolio
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
TOOLS = Path(__file__).resolve().parent
TAG_PATTERN = re.compile(r"^[a-z]{2,3}(-[A-Z][a-z]{3,4})?(-[A-Z]{2}|-[0-9]{3})?$")
PSEUDO_LOCALES = ("en-XA", "ar-XB")
PRODUCTION_COUNT = 16
FIRST_RESERVE = "cs-CZ"
UNREVIEWED = "UNREVIEWED: "
STYLE_FIELDS = (
    "tone", "capitalization", "button_labels", "ui_roles", "technical_terms",
    "contextual_terminology", "punctuation", "units", "natural_language",
)
PROMOTION_SURFACES = (
    ("Qt catalog", "i18n/app/{catalog}.ts synchronized from the English source"),
    ("Locale style guide", "i18n/style/{tag}.json authored and reviewed by a speaker"),
    ("Translation state", "i18n/state/translations.json shows no missing or stale message"),
    ("Installer mapping", "inno_language, inno_message_file, inno_language_name, "
                          "inno_language_id, inno_app_locale, inno_order, inno_source_kind, "
                          "inno_source_locale, and inno_fallback_locale set in i18n/locales.json"),
    ("Playnite mapping", "integrations/playnite/src/GameHQ.Playnite/Localization/locale-map.json "
                         "entry plus its XAML resource dictionary"),
    ("Launcher resources", "src/launcher/LauncherStrings.rc STRINGTABLE for the Win32 language"),
    ("Release notes", "assets/release-notes manifest and per-version documents"),
)


class LocaleError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise LocaleError(message)


def read_json(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"{path}: malformed UTF-8 or JSON: {error}")


def json_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def registry_path(root: Path) -> Path:
    return root / "i18n" / "locales.json"


def style_path(root: Path, tag: str) -> Path:
    return root / "i18n" / "style" / f"{tag}.json"


def load_registry(root: Path) -> dict[str, Any]:
    registry = read_json(registry_path(root))
    if not isinstance(registry.get("locales"), list) or not isinstance(
        registry.get("aliases"), dict
    ):
        fail("locale registry must contain locales and aliases")
    return registry


def entries(registry: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {entry["tag"]: entry for entry in registry["locales"]}


def require_locale(registry: dict[str, Any], tag: str) -> dict[str, Any]:
    entry = entries(registry).get(tag)
    if entry is None:
        fail(f"unknown locale: {tag}")
    return entry


def production_tags(registry: dict[str, Any]) -> list[str]:
    return [entry["tag"] for entry in registry["locales"]
            if entry.get("tier") == 1 and entry.get("state") == "enabled"]


def guard_pseudo(tag: str) -> None:
    if tag in PSEUDO_LOCALES:
        fail(f"{tag} is a development-only pseudo-locale and can never enter the registry")


def guard_portfolio(registry: dict[str, Any]) -> None:
    """The launch portfolio and the reserve contract are release gates."""
    production = production_tags(registry)
    if len(production) != PRODUCTION_COUNT:
        fail(f"production portfolio must stay at {PRODUCTION_COUNT} locales, found {len(production)}")
    for tag in PSEUDO_LOCALES:
        if tag in entries(registry):
            fail(f"{tag} must never appear in the production locale registry")
    reserve = entries(registry).get(FIRST_RESERVE)
    if reserve is None or reserve.get("state") != "reserve":
        fail(f"{FIRST_RESERVE} must remain the first reserve locale")


def catalog_name(tag: str) -> str:
    return "gamehq_" + tag.replace("-", "_")


def style_template(tag: str, english_name: str) -> dict[str, Any]:
    """A scaffold, explicitly marked unreviewed so nothing reads as authored."""
    guidance = {
        "tone": f"Describe the expected {english_name} tone for neutral software text.",
        "capitalization": f"Describe {english_name} capitalization rules for labels and headings.",
        "button_labels": f"Describe the conventional {english_name} form for button labels.",
        "ui_roles": "Describe how menus, settings, buttons, tooltips, and errors differ.",
        "technical_terms": "Describe how established product and platform terms are handled.",
        "contextual_terminology": "Resolve ambiguous GameHQ terms such as input, binding, "
                                  "capture, overlay, and replay from feature context.",
        "punctuation": f"Describe {english_name} quotation, spacing, and punctuation conventions.",
        "units": "Describe number, decimal separator, and unit conventions.",
        "natural_language": f"Describe idiomatic {english_name} syntax and calques to avoid.",
    }
    document = {
        "$schema": "../schema/locale-style.schema.json",
        "schema_version": 2,
        "locale": tag,
        "source_language": "en-US",
    }
    document.update({field: UNREVIEWED + guidance[field] for field in STYLE_FIELDS})
    return document


def style_is_authored(root: Path, tag: str) -> bool:
    path = style_path(root, tag)
    if not path.is_file():
        return False
    document = read_json(path)
    return not any(
        isinstance(document.get(field), str) and document[field].startswith(UNREVIEWED)
        for field in STYLE_FIELDS
    )


def run_tool(arguments: list[str], label: str) -> tuple[int, str]:
    """Compose an existing localization tool instead of duplicating its logic."""
    environment = dict(os.environ, PYTHONIOENCODING="utf-8")
    completed = subprocess.run(
        [sys.executable, *arguments], cwd=str(TOOLS), env=environment,
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    output = ((completed.stdout or "") + (completed.stderr or "")).strip()
    return completed.returncode, f"{label}: {output}"


def require_repository_root(root: Path, verb: str) -> None:
    """The generated-surface tools own their own repository paths, so the verbs
    that compose them only make sense against this checkout."""
    if root != ROOT:
        fail(f"`{verb}` inspects generated repository surfaces and cannot run with --root {root}")


def reconcile_state(root: Path, write: bool) -> tuple[int, str]:
    arguments = [str(TOOLS / "verify.py"), "--root", str(root)]
    if write:
        arguments.append("--update-state")
    return run_tool(arguments, "translation state")


# --------------------------------------------------------------------------- add


def plan_add(root: Path, args: argparse.Namespace) -> tuple[list[str], dict[str, bytes]]:
    tag = args.tag
    guard_pseudo(tag)
    if not TAG_PATTERN.fullmatch(tag):
        fail(f"{tag} is not a canonical BCP-47 locale tag")
    if not args.english_name or not args.native_name:
        fail(f"{tag} requires both --english-name and --native-name")
    registry = load_registry(root)
    guard_portfolio(registry)
    known = entries(registry)
    aliases = dict(registry["aliases"])
    catalog = catalog_name(tag)

    if tag in known:
        existing = known[tag]
        if existing.get("state") == "enabled":
            fail(f"{tag} already exists as an enabled production locale")
        return [f"{tag} is already registered as a {existing['state']} locale; nothing to add"], {}

    if any(entry.get("qt_catalog") == catalog for entry in registry["locales"]):
        fail(f"catalog {catalog} is already mapped to another locale")
    if tag in aliases:
        fail(f"{tag} is already an alias of {aliases[tag]}")
    for alias in args.alias:
        if alias in known:
            fail(f"alias {alias} collides with the existing locale {alias}")
        if alias in aliases and aliases[alias] != tag:
            fail(f"alias {alias} is already mapped to {aliases[alias]}")
        if alias == tag:
            fail(f"alias {alias} cannot repeat its own locale tag")
    if len(set(args.alias)) != len(args.alias):
        fail("duplicate alias passed more than once")
    if args.fallback not in known:
        fail(f"fallback locale {args.fallback} is not registered")

    entry = {
        "tag": tag,
        "names": {"native": args.native_name, "english": args.english_name},
        "aliases": sorted(args.alias),
        "tier": 0,
        "state": "reserve",
        "direction": args.direction,
        "fallback": args.fallback,
        "qt_catalog": catalog,
        "inno_language": None,
        "inno_message_file": None,
        "completeness_policy": "complete_at_release",
    }
    updated = json.loads(json.dumps(registry))
    updated["locales"].append(entry)
    for alias in sorted(args.alias):
        updated["aliases"][alias] = tag
    updated["aliases"] = {key: updated["aliases"][key] for key in sorted(updated["aliases"])}

    actions = [
        f"register {tag} as a disabled reserve locale (tier 0, state reserve, not release-eligible)",
        f"map catalog {catalog} and fallback {args.fallback}",
        f"scaffold i18n/style/{tag}.json as UNREVIEWED guidance",
        "reconcile i18n/state/translations.json",
    ]
    if args.alias:
        actions.insert(2, f"add aliases: {', '.join(sorted(args.alias))}")
    files = {
        registry_path(root).as_posix(): json_bytes(updated),
        style_path(root, tag).as_posix(): json_bytes(style_template(tag, args.english_name)),
    }
    return actions, files


def command_add(root: Path, args: argparse.Namespace) -> int:
    actions, files = plan_add(root, args)
    for action in actions:
        print(f"{'would ' if args.check else ''}{action}")
    if args.check or not files:
        if files:
            print("dry run: no file was written")
        return 0
    for path, payload in files.items():
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
    code, message = reconcile_state(root, write=True)
    print(message)
    if code != 0:
        fail("translation state could not be reconciled for the new locale")
    print()
    print(f"{args.tag} is registered and NOT release-eligible. Promotion requires:")
    for name, requirement in PROMOTION_SURFACES:
        print(f"  - {name}: {requirement.format(tag=args.tag, catalog=catalog_name(args.tag))}")
    print("Promotion to an enabled production locale is an owner release decision.")
    return 0


# ------------------------------------------------------------------ lifecycle state


def rewrite_entry(root: Path, tag: str, changes: dict[str, Any], check: bool) -> dict[str, Any]:
    registry = load_registry(root)
    guard_portfolio(registry)
    entry = require_locale(registry, tag)
    guard_pseudo(tag)
    updated = json.loads(json.dumps(registry))
    for candidate in updated["locales"]:
        if candidate["tag"] == tag:
            candidate.update(changes)
    if not check:
        registry_path(root).write_bytes(json_bytes(updated))
    return entry


def guard_not_production(entry: dict[str, Any], verb: str) -> None:
    if entry.get("tier") == 1 and entry.get("state") == "enabled":
        fail(
            f"{entry['tag']} is a launch locale; {verb} would drop the portfolio below "
            f"{PRODUCTION_COUNT} locales. Withdrawing a launch locale is an owner release "
            "decision documented in docs/localization/CONTRIBUTING.md."
        )


def command_disable(root: Path, args: argparse.Namespace) -> int:
    registry = load_registry(root)
    entry = require_locale(registry, args.tag)
    guard_not_production(entry, "disabling")
    if entry.get("state") == "disabled":
        print(f"{args.tag} is already disabled")
        return 0
    rewrite_entry(root, args.tag, {"state": "disabled", "tier": 0}, args.check)
    print(f"{'would disable' if args.check else 'disabled'} {args.tag}")
    print("its Qt catalog, style guide, and translation provenance are untouched")
    return 0


def command_retire(root: Path, args: argparse.Namespace) -> int:
    registry = load_registry(root)
    entry = require_locale(registry, args.tag)
    guard_not_production(entry, "retiring")
    rewrite_entry(root, args.tag, {
        "state": "disabled", "tier": 0, "completeness_policy": "never_embed",
    }, args.check)
    print(f"{'would retire' if args.check else 'retired'} {args.tag}")
    print("completeness_policy is never_embed, so it can no longer be packaged")
    print("its Qt catalog, style guide, and reviewed translation history are kept")
    return 0


def command_restore(root: Path, args: argparse.Namespace) -> int:
    registry = load_registry(root)
    entry = require_locale(registry, args.tag)
    if entry.get("state") == "enabled":
        fail(f"{args.tag} is already an enabled production locale")
    rewrite_entry(root, args.tag, {
        "state": "reserve", "tier": 0, "completeness_policy": "complete_at_release",
    }, args.check)
    print(f"{'would restore' if args.check else 'restored'} {args.tag} as a reserve locale")
    print("restore never re-enables a locale directly; promotion stays an owner decision")
    return 0


# ------------------------------------------------------------------------- review


def command_review(root: Path, args: argparse.Namespace) -> int:
    registry = load_registry(root)
    entry = require_locale(registry, args.tag)
    state = read_json(root / "i18n" / "state" / "translations.json")
    locale_state = state["locales"].get(args.tag)
    if locale_state is None:
        fail(f"{args.tag} has no translation state; run `locale.py update {args.tag}` first")
    counts: dict[str, int] = {}
    provenance: dict[str, int] = {}
    for message in locale_state["messages"].values():
        counts[message["status"]] = counts.get(message["status"], 0) + 1
        kind = message["provenance"]["kind"]
        provenance[kind] = provenance.get(kind, 0) + 1
    print(f"{args.tag} ({entry['names']['english']}) state={entry['state']} tier={entry['tier']}")
    print(f"  catalog present: {locale_state['catalog_present']}")
    for status in sorted(counts):
        print(f"  status {status}: {counts[status]}")
    for kind in sorted(provenance):
        print(f"  provenance {kind}: {provenance[kind]}")
    print(f"  style guide authored: {style_is_authored(root, args.tag)}")
    print(f"  release-eligible: {entry.get('state') == 'enabled' and entry.get('tier') == 1}")
    print("Generation never marks a message reviewed; only a human_reviewed provenance does.")
    reviewed = counts.get("human_reviewed", 0)
    total = sum(counts.values())
    print(f"  human-reviewed coverage: {reviewed}/{total}")
    return 0


# ------------------------------------------------------------------------ update


GENERATED_SURFACES = (
    ("installer languages", "generate_inno_languages.py"),
    ("installer custom messages", "generate_inno_custom_messages.py"),
    ("release-note bundles", "generate_release_notes.py"),
    ("release publication", "generate_release_publication.py"),
)


def surface_commands(check: bool) -> list[tuple[str, list[str]]]:
    suffix = ["--check"] if check else []
    return [(label, [str(TOOLS / script), *suffix]) for label, script in GENERATED_SURFACES]


def command_update(root: Path, args: argparse.Namespace) -> int:
    require_repository_root(root, "update")
    registry = load_registry(root)
    require_locale(registry, args.tag)
    failures = 0
    code, message = reconcile_state(root, write=not args.check)
    print(message)
    failures += 1 if code != 0 else 0
    for label, arguments in surface_commands(args.check):
        code, message = run_tool(arguments, label)
        print(message)
        failures += 1 if code != 0 else 0
    if failures:
        fail(f"{failures} generated surface(s) are stale or invalid")
    return 0


# ------------------------------------------------------------------------ verify


def command_verify(root: Path, args: argparse.Namespace) -> int:
    require_repository_root(root, "verify")
    registry = load_registry(root)
    guard_portfolio(registry)
    print(f"portfolio: {PRODUCTION_COUNT} production locales, {FIRST_RESERVE} reserved, "
          "no pseudo-locale registered")
    failures = 0
    code, message = reconcile_state(root, write=False)
    print(message)
    failures += 1 if code != 0 else 0
    for label, arguments in surface_commands(True):
        code, message = run_tool(arguments, label)
        print(message)
        failures += 1 if code != 0 else 0
    for tag in production_tags(registry):
        if not style_is_authored(root, tag):
            print(f"style guide is missing or unreviewed for production locale {tag}")
            failures += 1
    if failures:
        fail(f"{failures} localization check(s) failed")
    print("all localization checks passed offline")
    return 0


# ----------------------------------------------------------------------- package


def command_package(root: Path, args: argparse.Namespace) -> int:
    require_repository_root(root, "package")
    registry = load_registry(root)
    guard_portfolio(registry)
    playnite = read_json(
        root / "integrations/playnite/src/GameHQ.Playnite/Localization/locale-map.json"
    )
    mapped = {entry["gamehq"] for entry in playnite["locales"]}
    launcher = (root / "src/launcher/LauncherStrings.rc").read_text(encoding="utf-8")
    failures = 0
    for entry in sorted(registry["locales"], key=lambda value: value["tag"]):
        tag = entry["tag"]
        eligible = entry.get("state") == "enabled" and entry.get("tier") == 1
        if not eligible:
            print(f"{tag}: not release-eligible ({entry['state']}, tier {entry['tier']})")
            continue
        problems = []
        if tag not in mapped:
            problems.append("no Playnite mapping")
        if entry.get("inno_language") is None:
            problems.append("no installer language mapping")
        if not (root / "i18n" / "app" / f"{entry['qt_catalog']}.ts").is_file():
            problems.append("no Qt catalog")
        if not style_is_authored(root, tag):
            problems.append("style guide missing or unreviewed")
        if problems:
            failures += 1
            print(f"{tag}: NOT packageable - {'; '.join(problems)}")
        else:
            print(f"{tag}: packageable")
    if launcher.count("STRINGTABLE") < PRODUCTION_COUNT:
        failures += 1
        print("src/launcher/LauncherStrings.rc has fewer string tables than production locales")
    code, message = run_tool(
        [str(TOOLS / "verify.py"), "--root", str(root), "--release"], "release gate"
    )
    print(message)
    failures += 1 if code != 0 else 0
    if failures:
        fail(f"{failures} locale(s) are not ready to package")
    return 0


def command_status(root: Path, args: argparse.Namespace) -> int:
    registry = load_registry(root)
    for entry in sorted(registry["locales"], key=lambda value: value["tag"]):
        eligible = entry.get("state") == "enabled" and entry.get("tier") == 1
        print(f"{entry['tag']:<8} tier={entry['tier']} state={entry['state']:<8} "
              f"release-eligible={'yes' if eligible else 'no':<3} "
              f"{entry['names']['english']}")
    return 0


COMMANDS = {
    "add": command_add,
    "update": command_update,
    "review": command_review,
    "disable": command_disable,
    "retire": command_retire,
    "restore": command_restore,
    "verify": command_verify,
    "package": command_package,
    "status": command_status,
}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--root", type=Path, default=ROOT)
    subparsers = parser.add_subparsers(dest="command", required=True)

    add = subparsers.add_parser("add", help="scaffold a new disabled reserve locale")
    add.add_argument("tag")
    add.add_argument("--english-name")
    add.add_argument("--native-name")
    add.add_argument("--direction", choices=("ltr", "rtl"), default="ltr")
    add.add_argument("--fallback", default="en-US")
    add.add_argument("--alias", action="append", default=[])
    add.add_argument("--check", action="store_true")

    for name, help_text in (
        ("update", "refresh the generated surfaces a locale owns"),
        ("disable", "stop offering a locale, keeping its work"),
        ("retire", "park a locale so it can never be packaged"),
        ("restore", "return a disabled or retired locale to reserve"),
    ):
        command = subparsers.add_parser(name, help=help_text)
        command.add_argument("tag")
        command.add_argument("--check", action="store_true")

    review = subparsers.add_parser("review", help="report translation and promotion readiness")
    review.add_argument("tag")

    subparsers.add_parser("verify", help="run the deterministic offline localization checks")
    subparsers.add_parser("package", help="report release and packaging eligibility")
    subparsers.add_parser("status", help="print the current portfolio")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    return COMMANDS[args.command](Path(args.root).resolve(), args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except LocaleError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
