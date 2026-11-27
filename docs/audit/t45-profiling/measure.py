import csv
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

import psutil


def timer_contexts(executable):
    for run in range(1, 4):
        for mode in ("timers-off", "timers-on"):
            command = [executable, mode]
            with subprocess.Popen(command, stdout=subprocess.PIPE, text=True,
                                  creationflags=subprocess.CREATE_NO_WINDOW) as child:
                process = psutil.Process(child.pid)
                time.sleep(0.7)
                start = time.perf_counter()
                before = process.num_ctx_switches().voluntary
                time.sleep(2.1)
                end = time.perf_counter()
                after = process.num_ctx_switches().voluntary
                output, _ = child.communicate(timeout=10)
                if child.returncode:
                    raise RuntimeError((mode, child.returncode))
                print(f"{run} {mode}: {after - before} Windows context switches over "
                      f"{end - start:.3f}s. {output.strip()}", flush=True)


if len(sys.argv) == 3 and sys.argv[1] == "--timer-contexts":
    timer_contexts(sys.argv[2])
    sys.exit(0)

out, build, source, qt = map(Path, sys.argv[1:5])
live_pid = int(sys.argv[5])
mib = 1024**2
seconds = 6
step = 0.10
qt_test = qt / "bin" / "qmltestrunner.exe"
scenarios = {
    "replay-synthetic-av": [build / "tst_segmentrecorder.exe", "silentStretchKeepsAudioContinuousAndInSync", "captureGapKeepsTheVideoTimeline"],
    "replay-hw-720p": [build / "tst_segmentrecorder.exe", "reusedEncoderWritesIndependentSegments"],
    "overlay-offscreen": [qt_test, "-input", source / "tests/qml/tst_overlaylayout.qml",
                          "-import", source / "tests/qml/imports", "-platform", "offscreen"],
}
env = dict(os.environ, GAMEHQ_AUDIO_E2E="0", QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
           TEMP=str(out), TMP=str(out), QT_FORCE_STDERR_LOGGING="1")
rows = []
thread_rows = []


def snapshot(p):
    with p.oneshot():
        memory = p.memory_info()
        cpu = p.cpu_times()
        return (time.perf_counter(), cpu.user + cpu.system, memory.rss / mib, memory.private / mib,
                p.num_ctx_switches().voluntary, len(p.threads()), p.num_handles())


def threads(p):
    return {t.id: t.user_time + t.system_time for t in p.threads()}


def measured(name, run, p, child=None):
    started = time.perf_counter()
    samples = []
    first_threads = {}
    last_threads = {}
    while (child is None and time.perf_counter() - started < seconds) or (child is not None and child.poll() is None):
        try:
            samples.append(snapshot(p))
            if not first_threads:
                first_threads = threads(p)
            last_threads = threads(p)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            break
        if child is not None and time.perf_counter() - started > 50:
            child.kill()
            child.wait()
            raise TimeoutError(name)
        time.sleep(step)
    code = child.wait() if child else None
    elapsed = time.perf_counter() - started
    assert len(samples) > 1, (name, run, code)
    first, last = samples[0], samples[-1]
    span = last[0] - first[0]
    row = dict(scenario=name, run=run, pid=p.pid, wall_s=round(elapsed, 3),
               sample_span_s=round(span, 3), samples=len(samples),
               cpu_pct_one_core=round((last[1] - first[1]) / span * 100, 2),
               working_set_peak_mib=round(max(s[2] for s in samples), 2),
               private_peak_mib=round(max(s[3] for s in samples), 2),
               context_switches_per_s=round((last[4] - first[4]) / span, 1),
               threads_peak=max(s[5] for s in samples), handles_peak=max(s[6] for s in samples),
               exit_code=code)
    rows.append(row)
    for thread_id, value in last_threads.items():
        dt = value - first_threads.get(thread_id, 0)
        if dt > 0:
            thread_rows.append(dict(scenario=name, run=run, thread_id=thread_id,
                                    cpu_s=round(dt, 6), cpu_pct_one_core=round(dt / span * 100, 2)))
    print(row, flush=True)


if live_pid:
    live = psutil.Process(live_pid)
    for run in range(1, 4):
        measured("installed-passive-unknown-activity", run, live)
for name, command in scenarios.items():
    for run in range(1, 4):
        log = out / f"{name}-{run}.log"
        with log.open("wb") as stderr:
            test = subprocess.Popen([str(x) for x in command] + ["-o", str(log.with_suffix(".qt.txt")) + ",txt"],
                                    env=env, cwd=out, stdout=stderr, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            if name == "overlay-offscreen":
                start = time.perf_counter()
                code = test.wait(timeout=20)
                rows.append(dict.fromkeys(rows[0]) | dict(
                    scenario=name, run=run, pid=test.pid,
                    wall_s=round(time.perf_counter()-start, 3), exit_code=code))
                if code:
                    raise RuntimeError((name, run, code))
            else:
                measured(name, run, psutil.Process(test.pid), test)
for name, table in (("samples.csv", rows), ("thread-cpu.csv", thread_rows)):
    with (out / name).open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(table[0]))
        writer.writeheader()
        writer.writerows(table)
for name in dict.fromkeys(row["scenario"] for row in rows):
    block = [x for x in rows if x["scenario"] == name]
    print(name, {key: (min(values), statistics.median(values), max(values))
                 for key in ("cpu_pct_one_core", "private_peak_mib", "context_switches_per_s", "wall_s")
                 if (values := [r[key] for r in block if r[key] is not None])}, flush=True)
