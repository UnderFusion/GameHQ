# External controller-input receiver (developer tool, cpo-o06d)

`gamehq_input_receiver.exe` watches the controller **from outside GameHQ** and
records, phase by phase, what actually reached a second process.

It exists because of a limit GameHQ cannot get around from the inside: while the
overlay holds the verified foreground and GameHQ has asked the GameInput runtime
for its exclusive-foreground policy, GameHQ's own logs can only report **what it
asked for** — never what the runtime delivered to somebody else. The question
"did the game stop receiving the pad" is a question about another process, so it
needs another process to answer it.

It is developer-only and never ships: the target is defined inside
`GAMEHQ_BUILD_TESTS` in `tests/CMakeLists.txt`, it lives outside every packaging
payload, and the build produces a single statically linked console exe that can
be copied anywhere.

## What it does and does not do

Observes only. There is no DLL injection, no game-process hook, no game memory
access, no virtual controller, no HidHide/ViGEm, no kernel or filter driver and
no per-game shim. It is a plain input consumer — which is the point: what a plain
input consumer sees is what a game sees.

Three observation paths, deliberately:

| Path | What it sees | Why it is here |
|---|---|---|
| `gameinput` | A second GameInput client in another process | The path `cpo-o06c`'s exclusive request is about |
| `xinput` | Polled `XInputGetState` / `XInputGetStateEx` | What most games actually use; GameInput's policy cannot touch it, so it defines the compatibility boundary |
| `rawinput` | A `RIDEV_INPUTSINK` background sink | The strongest background observation Windows offers for the same HID device |

Two details matter for honesty:

* The GameInput path asks the runtime for **background ordinary input**
  (`EnableBackgroundInput | EnableBackgroundGuideButton |
  EnableBackgroundShareButton`). Without that request a gap would only prove that
  the receiver was not the foreground app — the receiver always logs the policy
  it asked for.
* **Activity and change are counted separately.** A pad that streams reports
  while idle proves the delivery path is alive; a button edge proves the payload
  arrived. A provider whose baseline saw no activity at all is reported as
  `not-measurable`, never as isolation.

## Running it

```
out-tests\gamehq_input_receiver.exe --log receipt.log --duration 15
out-tests\gamehq_input_receiver.exe --log receipt.log --self-check
out-tests\gamehq_input_receiver.exe --help
```

| Option | Meaning |
|---|---|
| `--log <file>` | Where the receipt goes (required, flushed per line) |
| `--duration <s>` | Bounded run; `0` (default) runs until Ctrl+C or `--stop-on-stdin` |
| `--providers a,b,c` | `gameinput`, `xinput`, `rawinput` (default: all three) |
| `--phase-file <file>` | Each new non-empty line names the next phase |
| `--verdict-phases a,b,c` | The triple the verdict compares (default `baseline,exclusive,restored`) |
| `--raw-usages page:usage,…` | Raw Input top-level collections to register (default `01:04,01:05,01:08`) |
| `--label <text>` | Free-form label stored in the header |
| `--stop-on-stdin` | Stop when stdin closes — how the automated experiment ends a run |
| `--self-check` | Attach every provider, report, exit (no device needed) |

## The receipt

One line per event, `key=value` tokens, values quoted when they contain spaces —
so `tools/input-receiver/ReceiverModel.h`'s parser (and any script) can read it:

```
t=0.681s event=presence provider=gameinput device=054c:0ce6 state=attached name="PS5 Controller"
t=1.041s event=activity provider=gameinput interval=0.502s arrivals=91 transitions=1 ... self=background fg=0x012f067c fgpid=60216 fgtitle="..."
t=4.102s event=phase name=exclusive
t=13.15s event=summary provider=gameinput phase=exclusive duration=5.004s arrivals=0 transitions=0 rate=0.00/s
t=13.15s event=verdict provider=gameinput baselineRate=213.36/s exclusiveRate=0.80/s restoredRate=248.93/s result=blocked-then-resumed note="arrivals stopped while the exclusive policy was in force and returned after release"
```

Every `activity`, `presence` and `phase` line carries `self=` (the receiver's own
foreground state) and `fg=`/`fgpid=`/`fgtitle=` (who owned the foreground at that
moment). A gap that lines up with the receiver *losing* the foreground is a
different story from a gap while it stayed in the background, and the receipt
lets a reader tell them apart instead of trusting the summary word.

Verdicts are per provider:

| Verdict | Meaning |
|---|---|
| `blocked-then-resumed` | Baseline had activity, the exclusive phase had (almost) none, and it came back after release |
| `continuous` | Activity kept flowing during the exclusive phase — not blocked for this provider |
| `not-measurable` | The baseline saw nothing for this provider: the arrangement proves nothing here |
| `blocked-no-resume` | Went quiet and stayed quiet (a device change or an unplugged pad does this) — suspect, not proof |

## Measured on this machine (2026-09-22, DualSense 054c:0ce6 over USB)

Two independent runs of the automated experiment
(`tests/tst_inputreceiver.cpp::externalReceiverMeasuresTheExclusivePolicy`),
which drives the **shipped** policy path — `GameInputFocusController` asking
`ProductionGameInputApi`, the same call `cpo-o06c` wired into the app — while a
test-owned window holds the foreground and the receiver watches from outside:

| Run | baseline | exclusive | restored | verdict |
|---|---|---|---|---|
| 1 | 208.90/s (728) | **0.00/s (0)** | 248.94/s (885) | `blocked-then-resumed` |
| 2 | 208.73/s (732) | **0.00/s (0)** | 248.88/s (882) | `blocked-then-resumed` |

So on this machine and this pad, the exclusive-foreground policy really does stop
**another process's** GameInput delivery, and releasing it really does restore
it. Those numbers are exactly the evidence `cpo-o06c` could not produce from
inside GameHQ.

What the same runs also say, and must not be hidden:

* `xinput` and `rawinput` were **not measurable** in the automated run — the
  DualSense is not an XInput device, and an *idle* pad produces no Raw Input
  reports for the registered collections. Their rows are `not-measurable`, not
  "isolated".
* Nothing here says what a game doing its own HID reads sees. A pad that streams
  on a vendor collection, a DirectInput reader or a game with an XInput device
  are outside what this measurement covered.

## Running the real-game experiment by hand

`Run-IsolationExperiment.ps1` starts the receiver, walks the operator through the
three phases and prints the verdicts — use it with GameHQ and a real game on
screen (the automated test above proves the policy path; this proves the same
thing while a real game is running):

```powershell
powershell -ExecutionPolicy Bypass -File tools\input-receiver\Run-IsolationExperiment.ps1 `
    -Receiver out-tests\gamehq_input_receiver.exe `
    -LogPath .claude-gui-temp\cpo-o06d-manual-receipt.log
```

Press a button on the pad during **every** phase, including the exclusive one:
the whole measurement is "did the pad still reach an outside process", and only
the pad can answer that.
