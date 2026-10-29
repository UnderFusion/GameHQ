"""Windows-only full-application updater lifecycle acceptance, in a disposable repo folder.

Uses the test-trust updater twin with a signed full candidate payload; never
substitutes a health-token fixture or manually replaces the live application.
psutil is used only to identify and wait for the exact disposable executable.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import time
import zipfile

import psutil

REPO = Path(__file__).resolve().parents[1]
OWNED_ROOTS: set[Path] = set()


def scoped(value: str | Path) -> Path:
    path = Path(value).resolve()
    if not path.is_relative_to(REPO) or path == REPO:
        raise ValueError(f"Acceptance paths must remain inside the repository: {path}")
    return path


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def app_process(root: Path) -> psutil.Process | None:
    executable = os.path.normcase(str(root / "app" / "GameHQ.exe"))
    for process in psutil.process_iter(["exe"]):
        try:
            if os.path.normcase(process.info["exe"] or "") == executable:
                return process
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    return None


def wait_for(predicate, seconds: float = 40):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.2)
    raise TimeoutError("Disposable application did not reach the expected lifecycle state")


def close_cleanly(root: Path) -> None:
    process = app_process(root)
    if not process:
        raise RuntimeError("Application exited before its orderly-close check")
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.PostThreadMessageW.argtypes = [wintypes.DWORD, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    sent = []
    gui_threads = []

    @callback_type
    def visit(window, _):
        pid = wintypes.DWORD()
        thread = user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
        if pid.value == process.pid:
            title = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(window, title, len(title))
            if title.value == "GameHQ":
                user32.PostMessageW(window, 0x0010, 0, 0)  # WM_CLOSE; QML calls quitApplication.
                sent.append(True)
            elif title.value == "QTrayIconMessageWindow":
                gui_threads.append(thread)
        return True

    user32.EnumWindows(visit, 0)
    if not sent:
        # A start-minimized QML window has no HWND until it is shown. Qt's
        # Windows dispatcher handles WM_QUIT on its GUI thread by quitting the
        # application normally; this exercises teardown without flashing UI.
        if not gui_threads or not user32.PostThreadMessageW(gui_threads[0], 0x0012, 0, 0):
            raise RuntimeError("Could not signal the disposable Qt GUI thread to quit")
    process.wait(timeout=40)
    if (root / "gamehq-data" / "session.marker").exists():
        raise RuntimeError("Orderly shutdown left the session marker behind")


def ordinary_start(root: Path, seconds: float = 12) -> None:
    phase = root / ".update" / "transaction.phase"
    completed = phase.exists() and phase.read_text().strip() in ("healthy", "rolled_back")
    subprocess.run([str(root / "GameHQ.exe")], cwd=root, check=True,
                   creationflags=subprocess.CREATE_NO_WINDOW)
    process = wait_for(lambda: app_process(root))
    if completed:
        # App initialization precedes the seven-second timer; elapsed process
        # lifetime alone does not establish that cleanup has had time to run.
        wait_for(lambda: not phase.exists() if process.is_running() else False, 45)
        close_cleanly(root)
        return
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if not process.is_running():
            raise RuntimeError("An ordinary start exited before the cleanup interval")
        time.sleep(0.2)
    close_cleanly(root)


def preserved(root: Path) -> dict:
    data = root / "gamehq-data"
    config = json.loads((data / "config.json").read_text(encoding="utf-8-sig"))
    with sqlite3.connect(data / "gamehq.db") as database:
        game = database.execute(
            "SELECT display_name, process_name FROM games WHERE process_name='hotfix-sentinel.exe'"
        ).fetchall()
        favorite = database.execute(
            "SELECT file_path,is_favorite,deleted_at FROM captures WHERE process_name='hotfix-sentinel.exe'"
        ).fetchall()
    return {"settings": {key: config.get(key) for key in
                         ("ui.language", "replay.length_seconds", "tray.close_to_tray")},
            "games": game, "favorites": favorite,
            "capture_sha256": digest(root / "Captures" / "Hotfix Sentinel" / "Screenshots" / "sentinel.png")}


def seed(root: Path) -> None:
    OWNED_ROOTS.add(root)
    data = root / "gamehq-data"
    data.mkdir(parents=True, exist_ok=True)
    write_json(data / "config.json", {"startup.minimized": True, "startup.enabled": False,
        "tray.close_to_tray": False, "ui.language": "en-US", "replay.length_seconds": 45,
        "updates.check_automatically": False, "replay.auto": False})
    result = subprocess.run([str(root / "app" / "GameHQ.exe"), "--smoke-test"], cwd=root,
                            timeout=50, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode or (data / "session.marker").exists():
        raise RuntimeError("Old application profile initialization failed")
    shot = root / "Captures" / "Hotfix Sentinel" / "Screenshots" / "sentinel.png"
    shot.parent.mkdir(parents=True, exist_ok=True)
    import base64
    shot.write_bytes(base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="))
    with sqlite3.connect(data / "gamehq.db") as database:
        database.execute("INSERT INTO games(display_name,process_name,created_at) VALUES(?,?,?)",
                         ("Hotfix Sentinel", "hotfix-sentinel.exe", "2026-10-29T00:00:00Z"))
        game = database.execute("SELECT id FROM games WHERE process_name='hotfix-sentinel.exe'").fetchone()[0]
        database.execute("INSERT INTO captures(file_path,type,game_id,process_name,created_at,is_favorite) "
                         "VALUES(?,?,?,?,?,1)", ("portable:/Captures/Hotfix Sentinel/Screenshots/sentinel.png", "screenshot", game,
                         "hotfix-sentinel.exe", "2026-10-29T00:00:00Z"))


def handoff(transaction: Path, helper: Path) -> None:
    # The helper observes the real authorizing process exit before applying.
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
    if not kernel32.GetProcessTimes(kernel32.GetCurrentProcess(), ctypes.byref(created),
            ctypes.byref(exited), ctypes.byref(system), ctypes.byref(user)):
        raise ctypes.WinError(ctypes.get_last_error())
    tx = json.loads(transaction.read_text(encoding="utf-8"))
    tx["callerPid"] = os.getpid()
    tx["callerCreationTime"] = (created.dwHighDateTime << 32) | created.dwLowDateTime
    write_json(transaction, tx)
    process = subprocess.Popen([str(helper), "--apply", str(transaction)],
        cwd=transaction.parents[1], creationflags=subprocess.CREATE_NO_WINDOW,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    (transaction.parent / "helper.pid").write_text(str(process.pid), encoding="ascii")
    # The caller can exit immediately; creation-time validation still pins its identity.


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-portable")
    parser.add_argument("--release-dir")
    parser.add_argument("--test-helper")
    parser.add_argument("--work")
    parser.add_argument("--version", default="0.8.1")
    parser.add_argument("--handoff")
    parser.add_argument("--fixture-setup", help="Installer compiled with isolated acceptance registry identity")
    args = parser.parse_args()
    if args.handoff:
        handoff(scoped(args.handoff), scoped(args.test_helper))
        return
    work = scoped(args.work)
    if work.exists():
        raise RuntimeError("Use a new disposable work folder; existing evidence is never deleted")
    work.mkdir(parents=True)
    temp = work / "temp"
    temp.mkdir()
    os.environ["TMP"] = os.environ["TEMP"] = str(temp)
    root = work / "GameHQ"
    with zipfile.ZipFile(scoped(args.old_portable)) as archive:
        for entry in archive.infolist():
            if not (root / entry.filename).resolve().is_relative_to(root.resolve()):
                raise RuntimeError("Portable archive entry escapes the disposable root")
        archive.extractall(root)
    seed(root)
    before = preserved(root)
    releases = scoped(args.release_dir)
    update = root / ".update"
    downloads = update / "downloads"
    downloads.mkdir(parents=True)
    for name in (f"GameHQ-{args.version}-win64-update.zip", "gamehq-release.json", "gamehq-release.sig"):
        shutil.copy2(releases / name, downloads / name)
    package = downloads / f"GameHQ-{args.version}-win64-update.zip"
    manifest = json.loads((downloads / "gamehq-release.json").read_text(encoding="utf-8"))
    tx = {"schemaVersion": 2, "productId": "underfusion.gamehq", "expectedVersion": args.version,
        "packageRoot": str(root), "packagePath": str(package), "stagingDir": str(update / "staging"),
        "backupDir": str(update / "backup"), "restartExecutable": str(root / "GameHQ.exe"),
        "healthTokenPath": str(update / "healthy.token"), "dataDir": str(root / "gamehq-data"),
        "dataSnapshotDir": str(update / "data-snapshot"), "callerPid": 1, "callerCreationTime": 1,
        "phase": "download_verified", "expectedSha256": digest(package),
        "manifestPath": str(downloads / "gamehq-release.json"),
        "signaturePath": str(downloads / "gamehq-release.sig"),
        "manifestSha256": digest(downloads / "gamehq-release.json"),
        "releaseKeyId": manifest["keyId"], "releaseSequence": manifest["releaseSequence"],
        "artifactName": package.name, "artifactSha256": digest(package), "artifactSize": package.stat().st_size,
        "releaseSignature": (downloads / "gamehq-release.sig").read_text(encoding="ascii").strip()}
    transaction = update / "transaction.json"
    write_json(transaction, tx)
    subprocess.run([sys.executable, str(Path(__file__).resolve()), "--handoff", str(transaction),
                    "--test-helper", str(scoped(args.test_helper))], check=True, timeout=30)
    wait_for(lambda: (update / "updater.log").exists())
    def applied():
        content = (update / "updater.log").read_text(encoding="utf-8", errors="replace")
        if "ERROR:" in content:
            raise RuntimeError(content)
        return "UPDATE APPLIED AND HEALTHY" in content
    wait_for(applied, 180)
    if (update / "transaction.phase").read_text().strip() != "healthy":
        raise RuntimeError("Actual updater transaction did not commit healthy")
    if not (update / "swap.manifest").exists():
        raise RuntimeError("Acceptance did not reach the original healthy plus retained journal state")
    close_cleanly(root)
    assert preserved(root) == before, "Actual updater changed seeded user data"
    print("PASS: actual signed full-payload transaction, post-update initialization and orderly exit", flush=True)
    for ordinal in (1, 2):
        ordinary_start(root)
        assert preserved(root) == before, "Ordinary startup changed seeded user data"
        print(f"PASS: ordinary launch {ordinal}, delayed cleanup and orderly exit", flush=True)
    assert not (update / "swap.manifest").exists()
    assert not (update / "transaction.phase").exists()
    assert (update / "updater.log").exists()
    checks = ["actual-update", "post-update", "ordinary-start-1", "ordinary-start-2",
              "settings", "favorite", "library-record", "capture-bytes", "delayed-cleanup"]
    if args.fixture_setup:
        # Independent installer-repair case. The lifecycle above has already
        # exercised the actual updater; this installs over a released affected
        # program layout, not over an artificially replaced candidate payload.
        affected = work / "affected-install"
        with zipfile.ZipFile(scoped(args.old_portable)) as archive:
            archive.extractall(affected)
        seed(affected)
        before_installer = preserved(affected)
        affected_update = affected / ".update"
        affected_update.mkdir()
        (affected_update / "transaction.phase").write_text("healthy\n", encoding="ascii")
        (affected_update / "swap.manifest").write_text("GAMEHQ_PROGRAM_SWAP_V1\n", encoding="ascii")
        write_json(affected_update / "transaction.json", tx)
        import winreg
        def registry_values(key_name):
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_name) as key:
                    return [winreg.EnumValue(key, i) for i in range(winreg.QueryInfoKey(key)[1])]
            except FileNotFoundError:
                return None
        production_keys = [r"Software\underfusion\GameHQ",
                           r"Software\Microsoft\Windows\CurrentVersion\App Paths\GameHQ.exe",
                           r"Software\Microsoft\Windows\CurrentVersion\Run"]
        production_before = {key: registry_values(key) for key in production_keys}
        identity_keys = [r"Software\underfusion\GameHQ-Hotfix081Acceptance",
                         r"Software\Microsoft\Windows\CurrentVersion\App Paths\GameHQ-Hotfix081Acceptance.exe",
                         r"Software\Microsoft\Windows\CurrentVersion\Uninstall\{83922597-224A-48B2-A501-4BB38CC21B16}_is1"]
        for key in identity_keys:
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key):
                    raise RuntimeError("Acceptance registry identity already exists; refusing to overwrite")
            except FileNotFoundError:
                pass
        try:
            subprocess.run([str(scoped(args.fixture_setup)), "/VERYSILENT", "/SUPPRESSMSGBOXES",
                "/NORESTART", "/SP-", "/NOICONS", f"/DIR={affected}",
                f"/LOG={work / 'installer-repair.log'}"], check=True, timeout=180)
            assert preserved(affected) == before_installer, "Installer changed seeded user data"
            ordinary_start(affected)
            assert preserved(affected) == before_installer, "Repaired launch changed seeded user data"
            assert not (affected_update / "swap.manifest").exists()
            print("PASS: hotfix installer over released healthy+journal layout preserves user data and starts", flush=True)
            checks.append("installer-repair-with-user-data")
        finally:
            # Only these dedicated, previously absent test keys are retired.
            for key in identity_keys:
                try:
                    winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key)
                except FileNotFoundError:
                    pass
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                        r"Software\Microsoft\Windows\CurrentVersion\Run", 0, winreg.KEY_SET_VALUE) as key:
                    winreg.DeleteValue(key, "GameHQ-Hotfix081Acceptance")
            except FileNotFoundError:
                pass
        assert {key: registry_values(key) for key in production_keys} == production_before, \
            "Installer acceptance modified a production registration"
    write_json(work / "result.json", {"passed": True, "version": args.version,
        "trust": "test-only helper with signed full application payload",
        "checks": checks,
        "before": before, "after": preserved(root)})


if __name__ == "__main__":
    try:
        main()
    finally:
        # A failed assertion must not leave an unattended disposable GUI.
        for root in OWNED_ROOTS:
            process = app_process(root)
            if process:
                try:
                    close_cleanly(root)
                except Exception:
                    process.terminate()
                    process.wait(timeout=15)
