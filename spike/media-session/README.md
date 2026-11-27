# Windows media-session feasibility

Windows exposes system media sessions through
`GlobalSystemMediaTransportControlsSessionManager`. These are sessions that
applications publish to the system. GameHQ does not discover players by
scanning processes or browser tabs. [Microsoft session manager documentation](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessionmanager)

| Capability | Scope and evidence |
| --- | --- |
| OS and permissions | Microsoft documents Windows 10 1809 (17763), Universal API Contract v7 and the `globalMediaControl` capability. The broker request was exercised from an unpackaged desktop process on Windows 11 build 26300. Older Windows versions and packaged policy were outside the test. |
| Session identity | Ask for available sessions when active and require explicit selection. Use the session's identity for commands. Multiple sessions can share an app ID. If the selected session disappears, require a new selection. |
| Play, Pause, Next and Previous | Microsoft exposes distinct capability flags and asynchronous `Try…Async` methods. An app can omit a command or refuse a request. The fixture verifies selection, capability gating and refusal handling. Application-specific command support remains unmeasured. |
| Metadata | The proof retains at most 16 session snapshots and 256 UTF-16 code units per text field. Artwork is omitted and no metadata is written to disk. Live title, artist, artwork and browser/player coverage need a disposable player session. |
| Lifecycle | Activate one event subscription and clear it on deactivate or shutdown. Invalidate stale snapshot generations and cancel outstanding commands on switch or disappearance. An already delivered command may still take effect. The fixture verifies this boundary without a desktop input or media source. |

The control contract is described by Microsoft's [session API](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssession)
and [playback capability flags](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessionplaybackcontrols).

## Disposable proofs

From a GameHQ build configured with `GAMEHQ_BUILD_TESTS=ON`, run the targets
below. Substitute the build directory used in your project-local toolchain
setup.

```powershell
cmake --build out-tests --target tst_mediasessionprototype media_session_broker_probe
ctest --test-dir out-tests -R '^tst_mediasessionprototype$' --output-on-failure
& ./out-tests/media_session_broker_probe.exe
```

The CTest target uses only a fake backend. The separate console probe invokes
`RequestAsync` and reports whether the broker returned a manager. It never
lists sessions, reads playback metadata, registers events or sends a command.
Its three-second async wait applies only to the standalone probe. The
prototype client is event-driven. Neither target is part of GameHQ.

The project-local MinGW 13.1 kit supplies the WinRT activation, string and
async interfaces. It lacks the `Windows.Media.Control` projection. The probe
uses a small ABI declaration for the manager request using the same style as
the WGC code. It adds no toolchain dependency. A future native backend would
need the additional ABI declarations for session enumeration, media
properties, transport controls and event tokens. Verify their GUIDs and
method order against the Windows metadata and test apartment marshaling
before connecting the backend to any panel. [Microsoft metadata bindings](https://github.com/microsoft/windows-rs/blob/master/crates/libs/windows/src/Windows/Media/Control/mod.rs)
