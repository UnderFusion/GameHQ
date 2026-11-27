"""Run CTest and preserve revision-bound suite and Qt case evidence."""

from __future__ import annotations

import argparse
from collections import Counter
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
QT_REPORT_SUFFIX = ",junitxml"
CACHE_KEYS = (
    "CMAKE_BUILD_TYPE", "CMAKE_GENERATOR", "CMAKE_CXX_COMPILER",
    "CMAKE_PREFIX_PATH", "CMAKE_MAKE_PROGRAM", "Qt6_DIR", "GAMEHQ_BUILD_TESTS",
    "GAMEHQ_RELEASE_TRUST_TEST_KEYS", "GAMEHQ_ENABLE_PSEUDO_LOCALES",
    "GAMEHQ_TELEGRAM_INTEGRATED",
)


@dataclass(frozen=True)
class Suite:
    name: str
    kind: str
    tests: tuple[str, ...] = ()


HEADLESS = (
    Suite("unit", "unit", (
        "tst_configmanager", "tst_gameidentity", "tst_inputpattern",
        "tst_logperf", "tst_logrotation", "tst_settingsapplypolicy",
        "tst_navigationstate", "tst_shutdownwait",
    )),
    Suite("integration", "integration", (
        "tst_captureacknowledgement", "tst_capturefilesync",
        "tst_controllerstatelifetime", "tst_gameinputdelivery", "tst_galleryscale",
        "tst_integrationclient", "tst_integrationprotocol", "tst_ipcbounds",
        "tst_portableprofileimporter", "tst_releasemanifest", "tst_releasetrust",
        "tst_thumbnailscheduler", "tst_updatertransaction",
    )),
    Suite("public-feed", "mock", ("tst_updatediscovery",)),
    Suite("synthetic", "synthetic", (
        "tst_gdireadback", "tst_overlaylayout", "tst_rawinputflood",
        "tst_segmentrecorder_synthetic",
    )),
)
CONCURRENCY = (
    "tst_integrationservice", "tst_integrationclient", "tst_integrationprotocol",
    "tst_capturepublisher", "tst_capturedatabaserepair", "tst_processidentity",
    "tst_portableprofileimporter", "tst_updatertransaction",
)


def timestamp() -> str:
    return datetime.now(timezone.utc).isoformat()


def atomic_json(path: Path, data: object) -> None:
    staging = path.with_suffix(".tmp")
    staging.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    staging.replace(path)


def git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=REPO, check=True, capture_output=True)
    return result.stdout.decode("utf-8", errors="replace").strip()


def read_cache(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"^([^:/][^:]*):[^=]+=(.*)$", line)
        if match:
            values[match[1]] = match[2]
    return values


def tool_version(exe: str) -> str:
    try:
        result = subprocess.run([exe, "--version"], check=True, capture_output=True, timeout=10)
        return result.stdout.decode("utf-8", errors="replace").splitlines()[0]
    except (OSError, subprocess.SubprocessError, IndexError):
        return "unavailable"


def file_identity(path: Path) -> dict[str, object]:
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"path": str(path), "sha256": digest.hexdigest(), "bytes": path.stat().st_size}


@contextmanager
def exclusive_reports(build: Path):
    # Qt report paths and CTest's own Testing directory belong to the build.
    lock_path = build / "Testing/gamehq-regression.lock"
    lock_path.parent.mkdir(exist_ok=True)
    with lock_path.open("a+b") as lock:
        if os.name == "nt":
            import msvcrt
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def configured_qt_version(cache: dict[str, str]) -> str | None:
    qt = cache.get("Qt6_DIR")
    if not qt:
        return None
    try:
        version = (Path(qt) / "Qt6ConfigVersionImpl.cmake").read_text(encoding="utf-8")
    except OSError:
        try:
            version = (Path(qt) / "Qt6ConfigVersion.cmake").read_text(encoding="utf-8")
        except OSError:
            return None
    match = re.search(r'set\(PACKAGE_VERSION\s+"([^"]+)"\)', version)
    return match[1] if match else None


def report_path(test: dict, report_root: Path) -> Path | None:
    for arg in test.get("command", []):
        if not arg.endswith(QT_REPORT_SUFFIX):
            continue
        path = Path(arg.removesuffix(QT_REPORT_SUFFIX)).resolve()
        if path.parent != report_root or path.suffix != ".xml":
            raise ValueError(f"Qt report path outside the generated report directory: {path}")
        return path
    return None


def load_ctest_cases(path: Path) -> dict[str, dict]:
    root = ET.parse(path).getroot()
    if root.tag not in ("testsuite", "testsuites"):
        raise ValueError("Expected a CTest JUnit report")
    cases = {}
    for entry in root.iter("testcase"):
        name = entry.attrib["name"]
        if name in cases:
            raise ValueError(f"Duplicate CTest result: {name}")
        failure = entry.find("failure")
        if failure is None:
            failure = entry.find("error")
        reason = None
        if entry.find("skipped") is not None:
            status = "skipped"
        elif failure is not None:
            reason = failure.get("message", "test failed")
            status = "timeout" if reason.casefold().startswith("timeout") else "failed"
        elif entry.get("status") == "run":
            status = "passed"
        else:
            status = "not_run"
        cases[name] = {"status": status, "seconds": float(entry.get("time", "0")), "reason": reason}
    return cases


def load_qt_counts(path: Path) -> dict[str, int]:
    root = ET.parse(path).getroot()
    if root.tag != "testsuite":
        raise ValueError("Expected a QtTest JUnit report")
    counts = {key: int(root.attrib[key]) for key in ("tests", "failures", "errors", "skipped")}
    if any(count < 0 for count in counts.values()):
        raise ValueError("Negative QtTest count")
    counts["passed"] = counts["tests"] - sum(counts[key] for key in ("failures", "errors", "skipped"))
    if counts["tests"] == 0 or counts["passed"] < 0:
        raise ValueError("Incomplete or inconsistent QtTest counts")
    return counts


def result_status(code: int, cases: dict, issues: list[str]) -> tuple[str, int]:
    statuses = {case["status"] for case in cases.values()}
    if "timeout" in statuses:
        return "timeout", code or 1
    if code:
        return "failed", code
    if issues or statuses & {"not_run", "failed"}:
        return "incomplete", 1
    if "skipped" in statuses or any(case.get("qt", {}).get("skipped") for case in cases.values()):
        return "passed_with_skips", 0
    return "passed", 0


def discover(command: list[str], cwd: Path, directory: Path) -> list[dict]:
    selection = command + ["--show-only=json-v1"]
    result = subprocess.run(selection, cwd=cwd, capture_output=True, check=False)
    (directory / "selection.json").write_bytes(result.stdout)
    (directory / "selection.stderr.log").write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(f"CTest selection failed ({result.returncode})")
    return json.loads(result.stdout)["tests"]


def run_suite(
    base: list[str], build: Path, directory: Path, suite: Suite, jobs: int, env: dict, run: dict,
    executables: dict, save,
) -> int:
    directory.mkdir()
    command = base.copy()
    if suite.tests:
        command.extend(["-R", "^(" + "|".join(re.escape(name) for name in suite.tests) + ")$"])
    run["selectionCommand"] = command + ["--show-only=json-v1"]
    tests = discover(command, build, directory)
    expected = {test["name"] for test in tests}
    run["expectedTests"] = sorted(expected)
    if not expected or (suite.tests and expected != set(suite.tests)):
        raise RuntimeError(f"Invalid test selection; missing {sorted(set(suite.tests) - expected)}")
    report_root = (build / "Testing/GameHQ-Qt").resolve()
    reports = {}
    for test in tests:
        if not test.get("command"):
            raise RuntimeError(f"CTest found no executable for {test['name']}. Build the selected targets first.")
        command_exe = test["command"][0]
        executable = command_exe if Path(command_exe).is_absolute() else shutil.which(
            command_exe, path=env.get("PATH")
        )
        if not executable or not Path(executable).is_file():
            raise RuntimeError(f"Test executable missing for {test['name']}: {command_exe}")
        executable = str(Path(executable).resolve())
        if executable not in executables:
            executables[executable] = file_identity(Path(executable))
        path = report_path(test, report_root)
        if path:
            if path in reports.values():
                raise RuntimeError(f"Two tests share Qt report {path}")
            path.unlink(missing_ok=True)
            reports[test["name"]] = path

    junit = directory / "ctest.xml"
    console = directory / "ctest.log"
    command.extend(["--output-on-failure", "--parallel", str(jobs), "--output-junit", str(junit)])
    run.update({"command": command, "workingDirectory": str(build), "startedUtc": timestamp()})
    save()
    started = time.monotonic()
    with console.open("wb") as log:
        code = subprocess.run(command, cwd=build, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    run.update({"exitCode": code, "seconds": round(time.monotonic() - started, 3), "finishedUtc": timestamp()})

    issues = run["evidenceIssues"]
    cases = {}
    try:
        cases = load_ctest_cases(junit)
    except (OSError, ET.ParseError, ValueError, KeyError) as error:
        issues.append(f"Missing or invalid CTest report: {error}")
    if expected != cases.keys():
        issues.append(f"CTest report differs from selection. Missing: {sorted(expected - cases.keys())}. "
                      f"Unexpected: {sorted(cases.keys() - expected)}.")
    if not console.stat().st_size:
        issues.append("Empty CTest console log.")
    qt_directory = directory / "qt"
    qt_directory.mkdir()
    for name, path in reports.items():
        case = cases.get(name)
        if not case or case["status"] in ("skipped", "not_run"):
            continue
        try:
            counts = load_qt_counts(path)
            (qt_directory / path.name).write_bytes(path.read_bytes())
            case["qt"] = counts
            if case["status"] == "passed" and counts["failures"] + counts["errors"]:
                issues.append(f"{name}: Qt recorded failures despite a passing CTest result.")
        except (OSError, ET.ParseError, ValueError, KeyError) as error:
            issues.append(f"{name}: missing or invalid Qt report: {error}")
    counts = Counter(case["status"] for case in cases.values())
    run["testCounts"] = {status: counts[status] for status in ("passed", "failed", "skipped", "timeout", "not_run")}
    qt_counts = [case["qt"] for case in cases.values() if "qt" in case]
    run["qtCounts"] = {
        "reports": len(qt_counts),
        **{key: sum(item[key] for item in qt_counts) for key in ("tests", "passed", "failures", "errors", "skipped")},
    }
    run["cases"] = cases
    run["status"], code = result_status(code, cases, issues)
    save()
    print(f"{suite.name}: {run['status']} {run['testCounts']} ({run['seconds']} s)", flush=True)
    if code:
        print(console.read_text(encoding="utf-8", errors="replace")[-6000:])
    return code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True, help="New evidence directory")
    parser.add_argument("--profile", choices=("headless", "concurrency", "ci"), default="headless")
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    build = args.build_dir.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipt = {
        "schemaVersion": 1, "status": "running", "profile": args.profile,
        "startedUtc": timestamp(), "runs": [],
        "environment": {"platform": platform.platform(), "python": platform.python_version()},
        "limitations": [
            "Automated evidence. Live WGC SDR/HDR, native in-game overlay, physical controllers, "
            "AMD capture, and live shutdown acceptance require separate receipts.",
            "Binary hashes identify the supplied build. Use a fresh checkout and a clean build for release evidence.",
        ],
    }

    def save():
        atomic_json(out / "receipt.json", receipt)

    save()
    code = 1
    run = None
    try:
        revision = git("rev-parse", "HEAD")
        receipt["source"] = {"revision": revision, "tree": git("rev-parse", "HEAD^{tree}")}
        if git("status", "--porcelain", "--untracked-files=no"):
            raise ValueError("Commit tracked changes before producing revision-bound evidence.")
        cache_file = build / "CMakeCache.txt"
        cache = read_cache(cache_file)
        if Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() != REPO:
            raise ValueError("Build directory belongs to another source checkout.")
        if cache.get("GAMEHQ_BUILD_TESTS") != "ON":
            raise ValueError("Build directory was configured without GameHQ tests.")
        if cache.get("GAMEHQ_RELEASE_TRUST_TEST_KEYS") != "OFF":
            raise ValueError("Production targets must use the production trust table.")
        ctest = cache["CMAKE_CTEST_COMMAND"]
        base = [ctest, "--test-dir", str(build)]
        if cache.get("CMAKE_BUILD_TYPE"):
            base.extend(["-C", cache["CMAKE_BUILD_TYPE"]])
        receipt["build"] = {
            "configuration": {key: cache.get(key) for key in CACHE_KEYS},
            "cmakeCache": file_identity(cache_file),
            "tools": {
                "ctest": tool_version(ctest),
                "cmake": tool_version(cache["CMAKE_COMMAND"]),
                "compiler": tool_version(cache["CMAKE_CXX_COMPILER"]),
                "buildTool": tool_version(cache["CMAKE_MAKE_PROGRAM"]),
                "qt": configured_qt_version(cache),
            },
            "products": [file_identity(path) for name in ("GameHQ", "GameHQLauncher", "GameHQUpdater")
                         if (path := build / (name + ".exe")).is_file()],
        }

        env = dict(os.environ)
        for key in ("GAMEHQ_PUBLIC_RELEASES_FIXTURE", "GAMEHQ_EXPECTED_STABLE_VERSION"):
            env.pop(key, None)
        receipt["environment"]["publicReleaseFeed"] = "tracked synthetic API fixture"
        if args.profile != "ci":
            scratch = out / "scratch"
            scratch.mkdir()
            env.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
                       GAMEHQ_AUDIO_E2E="0", TEMP=str(scratch), TMP=str(scratch))
        repeats = tuple(Suite(f"concurrency-{i}", "integration", CONCURRENCY) for i in range(1, 4))
        if args.profile == "headless":
            suites = HEADLESS
        elif args.profile == "concurrency":
            suites = repeats
        else:
            suites = (Suite("native", "ci-mixed"),) + repeats
        receipt["environment"]["testOptions"] = {
            key: env.get(key) for key in ("QT_QPA_PLATFORM", "QT_QUICK_BACKEND", "GAMEHQ_AUDIO_E2E")
        }
        receipt["plannedRuns"] = [{"name": suite.name, "kind": suite.kind} for suite in suites]
        executables = receipt["build"]["testExecutables"] = {}
        with exclusive_reports(build):
            for suite in suites:
                run = {"name": suite.name, "kind": suite.kind, "status": "running", "evidenceIssues": []}
                receipt["runs"].append(run)
                save()
                code = run_suite(base, build, out / suite.name, suite, args.jobs, env, run, executables, save)
                if code:
                    receipt["status"] = run["status"]
                    break
            else:
                receipt["status"] = ("passed_with_skips" if any(
                    entry["status"] == "passed_with_skips" for entry in receipt["runs"]
                ) else "passed")
        if git("rev-parse", "HEAD") != revision or git("status", "--porcelain", "--untracked-files=no"):
            raise ValueError("Tracked source changed during evidence collection.")
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        if run is not None and run["status"] == "running":
            run["status"] = "incomplete"
        receipt.update({"status": "incomplete", "error": str(error)})
        print(str(error), flush=True)
        code = code or 1
    finally:
        receipt.update({"finishedUtc": timestamp(), "exitCode": code})
        save()
    return code


if __name__ == "__main__":
    raise SystemExit(main())
