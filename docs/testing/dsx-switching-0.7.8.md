# DSX switching investigation and owner evidence

## Owner observations, 2026-09-22

Native wired DualSense: the owner reports PASS for game visibility without
minimization, overlay placement, controller navigation without underlying game
actions, close with no held-input leak, controller recovery, repeated cycles,
Alt-Tab without focus stealing, and disconnect/reconnect without stuck input.
No black screen or desktop flash was observed. These are owner observations,
not automated results. The report does not identify the tested executable hash,
game names, exact cycle count, firmware, Windows build, or every control used.
No preset, capture, alternate-port/profile, normal-exit, opening-gesture or
second-game result is inferred. The corresponding remaining checklist cases
remain unverified; passing native observations need not be repeated.

The abnormal-kill test was NOT RUN: opening Task Manager dismissed the overlay
and lost the exclusive-input precondition. The owner explicitly skipped further
testing and accepted residual risk. This is not a PASS or a crash-recovery proof.

## Existing log evidence

`build/gamehq-data/logs/gamehq.log`, 2026-09-22, contains 0.7.43 sessions.
At 12:46:37.494 GameInput attaches; at 12:46:38.483 it stops. Later overlay
opens refuse exclusive policy because no runtime is attached. The log does not
record why startup/session initialization failed. This session cannot be used
to attribute the owner's successful native observations to GameInput exclusivity.

At 13:42:24.050 WinMM sees Sony VID 054c/PID 05c4; it disappears at
13:42:25.303. At 13:42:25.894 WinMM sees 054c/0ce6, followed by Sony-provider
disconnect and WinMM selection. Earlier logs identify a DSX virtual DS4
3670/0902 as cloaked. These are observed identities, not proof that two endpoints
represent the same physical pad. Container/rekey evidence and the precise
user-visible failure are missing. No stale exclusive-policy leak is established:
the inspected opens explicitly say the policy was not in force.

## Diagnostic change

Provider lifecycle logging now includes startup/fallback reasons, hashed device,
logical, container, endpoint and root identities, match evidence, overlay/release
state, held count and attached runtime/policy. The diagnostic export retains the
last 32 rows; the rotating app log records the lifecycle chronology. There is
no per-input-report logging. Provider numbers follow ControllerProvider; match
numbers follow MatchEvidence in PhysicalControllerRegistry.h. Missing correlation
is reported as unavailable. No routing, focus, handoff or DSX settings are changed.

## Optional system-button callback correction

The wrapper treated device, reading and Guide/Share registration as mandatory.
Thus a failed Guide/Share registration discarded both successful registrations
and detached the focus-policy owner. Microsoft documents this registration as
specifically delivering Guide/Share events, separate from ordinary readings:
https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/input/gameinput/interfaces/igameinput/methods/igameinput_registersystembuttoncallback

The bounded fix retains device/read callbacks and the existing focus-policy
owner when only the optional registration fails. Required device/read failures
still shut down. The router withholds unavailable GameInput Guide/Share
capabilities, preserving existing eligible system-button providers rather than
advertising an undeliverable control. This does not create a fallback where no
provider exposes the button. Normal successful registration is unchanged, and a
fresh runtime session retries it. Ordinary GameInput readings retain their
existing shadow role; this does not replace standard-control arbitration.

Automated coverage checks retained readings, focus-policy restoration, clean
restart, mandatory registration failure, fallback edges and simulated physical
identity enrichment/rekey with virtual endpoint appearance/disappearance. The
simulation is not physical DSX acceptance.

A bounded production-API probe on 2026-09-22 successfully registered device and
reading callbacks while system registration returned `0x838ad029`. After that
failure it received 628 ordinary readings from Sony `054c:0ce6` (`PS5 Controller`)
over 2.5 seconds, kept the runtime loaded and shut down cleanly. An earlier
observation enumerated no usable controller and was inconclusive. No DSX process
was present in the accompanying process listings. The failure therefore is not
observed only during a DSX switch; this does not establish transport, hidden
driver state, the cause of the numeric HRESULT or the owner's DSX symptom.
Probe source and receipt: `.claude-gui-temp/dsx03-runtime-probe.cpp` and `.log`.

## One focused retest

Use the 0.7.8 package identified by the adjacent beta-manifest.json. Keep the
same DSX setup and game that showed the issue. Open the overlay, perform the
one DSX output/controller switch that failed, then close the overlay and try one
normal game input. Record the DSX mode before/after, whether the physical cable
stayed attached, the approximate time and exactly what stopped working or fired
unexpectedly. Return Copy diagnostic summary and that package's
`gamehq-data/logs/gamehq.log`. Stop after one reproduction. No native test repeat
is requested. DSX remains partial / needs verification.
