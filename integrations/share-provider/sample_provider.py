#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""GameHQ Share Provider API v1 - sample provider.

A complete, dependency-free example of an out-of-process Share destination. It
implements the public wire specification (docs/share-provider-api-v1.md) and
nothing else: no GameHQ headers, classes or libraries.

The destination is deliberately harmless: it "shares" a capture by copying it
into a folder on this PC (Inbox / Archive). Nothing is uploaded anywhere.

Requirements: Windows, Python 3.8+, standard library only.

    python sample_provider.py --dest-dir C:\\Temp\\ShareSample

GameHQ must have Settings > Capture > Share add-ons > "Allow add-ons from other
programs" switched on (then restarted). Start this program afterwards; it
connects to GameHQ, appears in the Share dialog as "Sample folder", and leaves
when you close it.

Exit codes: 0 clean exit, 2 rejected by GameHQ, 3 protocol incompatible,
4 GameHQ is not listening, 5 connection lost, 6 bad arguments.

The flags marked "test" exist so the conformance suite can drive the failure
paths a real provider must survive; a real provider would not have them.
"""

import argparse
import ctypes
import json
import os
import shutil
import struct
import sys
import tempfile
import time
from ctypes import wintypes

PIPE_NAME = "GameHQ.Share.Provider.v1"
MAX_FRAME = 64 * 1024          # protocol limit for one frame's payload
CHUNK = 64 * 1024              # copy granularity: progress + cancel checks


def log(text):
    sys.stderr.write("[sample-provider] " + text + "\n")
    sys.stderr.flush()


class Disconnected(Exception):
    """The pipe closed (GameHQ exited, restarted, or dropped us)."""


class ProtocolError(Exception):
    """GameHQ sent something that is not a valid frame."""


# --------------------------------------------------------------------------
# Transport: framing over a Windows named pipe.
#
# A named pipe opened without OVERLAPPED I/O serialises operations on the
# handle: a blocked read would also block our writes. So reads never block; we
# ask PeekNamedPipe how many bytes are waiting and only then read them. This
# lets a job send progress and still notice `job.cancel` between chunks.
# --------------------------------------------------------------------------
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.PeekNamedPipe.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(wintypes.DWORD),
]
_kernel32.PeekNamedPipe.restype = wintypes.BOOL


class Connection:
    def __init__(self, pipe_name, fragment_writes=False):
        import msvcrt
        self._file = open(r"\\.\pipe\%s" % pipe_name, "r+b", buffering=0)
        self._handle = msvcrt.get_osfhandle(self._file.fileno())
        self._buffer = b""
        self._fragment = fragment_writes

    def close(self):
        try:
            self._file.close()
        except OSError:
            pass

    def _available(self):
        avail = wintypes.DWORD(0)
        if not _kernel32.PeekNamedPipe(self._handle, None, 0, None, ctypes.byref(avail), None):
            raise Disconnected("pipe closed")
        return avail.value

    def _write_all(self, data):
        view = memoryview(data)
        try:
            while view:
                if self._fragment:
                    # Test: force many tiny OS writes so GameHQ must reassemble
                    # frames that arrive in pieces.
                    step = 1 + (len(data) - len(view)) % 7
                    written = self._file.write(view[:step])
                    self._file.flush()
                    time.sleep(0.001)
                else:
                    written = self._file.write(view)
                view = view[written:]
        except OSError:
            raise Disconnected("write failed")

    @staticmethod
    def frame(message):
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        if not payload or len(payload) > MAX_FRAME:
            raise ValueError("message does not fit in one frame")
        return struct.pack("<I", len(payload)) + payload

    def send(self, message):
        self._write_all(self.frame(message))

    def send_many(self, messages):
        """Several frames in ONE write, so the peer reads them together."""
        self._write_all(b"".join(self.frame(m) for m in messages))

    def _take_frame(self):
        if len(self._buffer) < 4:
            return None
        (length,) = struct.unpack_from("<I", self._buffer, 0)
        if length == 0 or length > MAX_FRAME:
            raise ProtocolError("bad frame length %d" % length)
        if len(self._buffer) < 4 + length:
            return None
        payload = self._buffer[4:4 + length]
        self._buffer = self._buffer[4 + length:]
        try:
            message = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            raise ProtocolError("frame is not valid UTF-8 JSON")
        if not isinstance(message, dict) or not isinstance(message.get("type"), str):
            raise ProtocolError("frame is not a message object")
        return message

    def receive(self, timeout=0.0):
        """Return the next message, or None if none arrived within `timeout`."""
        deadline = time.monotonic() + timeout
        while True:
            message = self._take_frame()
            if message is not None:
                return message
            waiting = self._available()
            if waiting:
                try:
                    chunk = self._file.read(min(waiting, 65536))
                except OSError:
                    raise Disconnected("read failed")
                if not chunk:
                    raise Disconnected("end of stream")
                self._buffer += chunk
                continue
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.005)


def connect(args):
    """Open the pipe, retrying while GameHQ is not (yet) listening."""
    deadline = time.monotonic() + args.connect_timeout
    delay = 0.1
    while True:
        try:
            return Connection(args.pipe, args.fragment_writes)
        except FileNotFoundError:
            reason = "GameHQ is not listening (are Share add-ons enabled?)"
        except OSError as error:            # e.g. ERROR_PIPE_BUSY
            reason = "cannot open the pipe: %s" % error
        if time.monotonic() >= deadline:
            raise ConnectionError(reason)
        time.sleep(delay)
        delay = min(delay * 2, 1.0)


# --------------------------------------------------------------------------
# The provider.
# --------------------------------------------------------------------------
class Sample:
    def __init__(self, args):
        self.args = args
        self.dest_dir = os.path.abspath(args.dest_dir)
        self.jobs_done = 0
        self.targets = [
            {"id": "inbox", "name": "Inbox", "kind": "external", "subtitle": "Folder on this PC"},
            {"id": "archive", "name": "Archive", "kind": "external", "subtitle": "Folder on this PC"},
        ]
        for i in range(args.many_targets):
            self.targets.append({"id": "extra%d" % i, "name": "Extra %d" % i, "kind": "external"})

    # -- manifest -----------------------------------------------------------
    def hello(self):
        provider = {
            "id": self.args.id,
            "name": self.args.name,
            "version": "1.0.0",
            "privacy": "Copies the capture into a folder on this PC. Nothing is uploaded.",
            "access": "none",
            "jobTimeoutMs": self.args.job_timeout_ms,
            "capabilities": ["image", "video", "target_search"] + self.args.extra_capability,
        }
        message = {
            "type": "hello",
            "requestId": "hello-1",
            "protocolMin": self.args.protocol_min,
            "protocolMax": self.args.protocol_max,
            "provider": provider,
        }
        if self.args.extra_fields:
            # Unknown optional fields must be ignored by GameHQ, not rejected.
            message["futureTopLevelField"] = {"anything": [1, 2, 3]}
            provider["futureProviderField"] = "ignored"
        return message

    # -- targets ------------------------------------------------------------
    def answer_targets(self, conn, request):
        query = str(request.get("query", "")).strip().lower()
        found = [t for t in self.targets if not query or query in t["name"].lower()]
        conn.send({"type": "targets.result", "requestId": request.get("requestId"),
                   "targets": found})

    # -- jobs ---------------------------------------------------------------
    def run_job(self, conn, job):
        """Handle one job.start. Returns the messages to send when finished."""
        args = self.args
        if args.crash_on_job:
            log("crashing on purpose (test)")
            os._exit(1)

        ident = {"jobId": job.get("jobId"), "token": job.get("token")}

        def result(outcome, code=None):
            message = dict(ident, type="job.result", outcome=outcome)
            if code:
                message["errorCode"] = code
            if args.extra_fields:
                message["futureResultField"] = True
            return message

        if args.silent_job:
            # Test: never answer. GameHQ's job timeout must end it as unconfirmed.
            log("staying silent (test)")
            while True:
                conn.receive(timeout=0.2)

        target = next((t for t in self.targets if t["id"] == job.get("targetId")), None)
        info = job.get("file")
        if target is None or not isinstance(info, dict):
            return [result("failed", "bad_job")]

        # The one and only file we are given. We open exactly this path,
        # read-only, and never look at anything next to it: the path is not
        # permission to browse the folder it sits in.
        source = info.get("path")
        name = os.path.basename(str(info.get("fileName", "")))
        if not isinstance(source, str) or not name or name != info.get("fileName"):
            return [result("failed", "bad_file")]
        folder = os.path.join(self.dest_dir, target["id"])
        destination = os.path.join(folder, name)
        try:
            os.makedirs(folder, exist_ok=True)
            total = os.path.getsize(source)
            if total != info.get("sizeBytes"):
                return [result("failed", "file_changed")]
            copied = 0
            with open(source, "rb") as src, open(destination, "wb") as dst:
                conn.send(dict(ident, type="job.progress", state="preparing"))
                while True:
                    chunk = src.read(CHUNK)
                    if not chunk:
                        break
                    dst.write(chunk)
                    copied += len(chunk)
                    conn.send(dict(ident, type="job.progress", state="transferring",
                                   progress=copied / total if total else 1.0))
                    if args.job_chunk_delay_ms:
                        time.sleep(args.job_chunk_delay_ms / 1000.0)
                    # Between chunks: has GameHQ asked us to stop?
                    while True:
                        message = conn.receive(timeout=0.0)
                        if message is None:
                            break
                        if message["type"] == "job.cancel" and message.get("jobId") == ident["jobId"] \
                                and message.get("token") == ident["token"]:
                            dst.close()
                            src.close()
                            os.remove(destination)     # nothing half-done is left behind
                            log("cancelled after %d of %d bytes" % (copied, total))
                            return [result("cancelled")]
                        if message["type"] == "targets.request":
                            self.answer_targets(conn, message)
        except OSError as error:
            log("copy failed: %s" % error)
            try:
                os.remove(destination)
            except OSError:
                pass
            return [result("failed", "copy_failed")]
        # `copied`: the capture is now in a folder; nothing was sent anywhere.
        return [result("copied")]

    # -- session ------------------------------------------------------------
    def session(self, conn):
        """One connection's lifetime. Returns an exit code."""
        args = self.args
        if args.hello_delay_ms:
            time.sleep(args.hello_delay_ms / 1000.0)   # test: miss the handshake window
        conn.send(self.hello())

        acked = False
        started = time.monotonic()
        while True:
            try:
                message = conn.receive(timeout=0.2)
            except ProtocolError as error:
                log("protocol error from GameHQ: %s" % error)
                return 5
            if message is None:
                if not acked and time.monotonic() - started > args.connect_timeout:
                    log("no hello.ack")
                    return 5
                continue
            kind = message["type"]

            if kind == "error":
                code = message.get("code")
                log("GameHQ error: %s" % code)
                if code == "protocol_incompatible":
                    return 3
                if not acked:
                    return 2
                continue          # violations are advisory; keep going
            if kind == "hello.ack":
                acked = True
                log("registered: protocol %s, accepted %s, ignored %s" % (
                    message.get("protocolSelected"), message.get("capabilities"),
                    message.get("ignoredCapabilities")))
                if args.send_unknown_type:
                    conn.send({"type": "future.thing", "x": 1})     # test: earns unknown_type
                continue
            if not acked:
                continue          # nothing else is valid before the ack

            if kind == "targets.request":
                self.answer_targets(conn, message)
            elif kind == "job.start":
                messages = self.run_job(conn, message)
                self.jobs_done += 1
                finished = args.exit_after_jobs and self.jobs_done >= args.exit_after_jobs
                if finished and args.coalesce_goodbye:
                    # Test: result + goodbye in a single write.
                    conn.send_many(messages + [{"type": "goodbye"}])
                    return 0
                for m in messages:
                    conn.send(m)
                if finished:
                    conn.send({"type": "goodbye"})
                    time.sleep(0.1)            # let the goodbye flush before closing
                    return 0
            # job.cancel with no job running, and any message type this
            # version does not know, are ignored on purpose: a newer GameHQ
            # may send more than this sample understands.

    def run(self):
        backoff = 0.2
        while True:
            try:
                conn = connect(self.args)
            except ConnectionError as error:
                log(str(error))
                if not self.args.reconnect:
                    return 4
                time.sleep(backoff)
                backoff = min(backoff * 2, 2.0)
                continue
            try:
                code = self.session(conn)
            except Disconnected as error:
                log("connection lost: %s" % error)
                code = 5
            finally:
                conn.close()
            if code == 0 or not self.args.reconnect or code in (2, 3):
                return code
            log("reconnecting")
            time.sleep(backoff)


def parse_args(argv):
    p = argparse.ArgumentParser(description="GameHQ Share Provider API v1 sample provider")
    p.add_argument("--dest-dir", default=os.path.join(tempfile.gettempdir(), "GameHQ-share-sample"),
                   help="folder the Inbox/Archive targets copy into")
    p.add_argument("--pipe", default=PIPE_NAME, help="pipe name (only tests change this)")
    p.add_argument("--id", default="ext.sample.folder")
    p.add_argument("--name", default="Sample folder")
    p.add_argument("--connect-timeout", type=float, default=10.0,
                   help="seconds to wait for GameHQ to listen / to send hello.ack")
    p.add_argument("--reconnect", action="store_true",
                   help="reconnect (with backoff) after GameHQ restarts or drops us")
    p.add_argument("--job-timeout-ms", type=int, default=20000)
    # -- test-only switches --
    p.add_argument("--protocol-min", type=int, default=1)
    p.add_argument("--protocol-max", type=int, default=1)
    p.add_argument("--extra-capability", action="append", default=[])
    p.add_argument("--extra-fields", action="store_true")
    p.add_argument("--send-unknown-type", action="store_true")
    p.add_argument("--fragment-writes", action="store_true")
    p.add_argument("--many-targets", type=int, default=0)
    p.add_argument("--hello-delay-ms", type=int, default=0)
    p.add_argument("--job-chunk-delay-ms", type=int, default=0)
    p.add_argument("--silent-job", action="store_true")
    p.add_argument("--crash-on-job", action="store_true")
    p.add_argument("--exit-after-jobs", type=int, default=0)
    p.add_argument("--coalesce-goodbye", action="store_true")
    return p.parse_args(argv)


def main(argv=None):
    try:
        args = parse_args(sys.argv[1:] if argv is None else argv)
    except SystemExit as exit_request:
        # argparse exits 0 for --help and 2 for bad arguments; 2 is already
        # taken ("rejected by GameHQ"), so bad arguments are reported as 6.
        return 0 if exit_request.code in (0, None) else 6
    if os.name != "nt":
        log("this sample needs Windows named pipes")
        return 6
    try:
        return Sample(args).run()
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
