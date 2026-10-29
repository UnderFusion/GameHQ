# Controller reporter response drafts

## Issue #32 — DS4Windows (send after 0.8.1 is published)

Thanks for the detailed report. GameHQ 0.8.1 adds a separate **Use Back/View as
Share** switch in Settings > Input for the selected eligible controller source.
Please select the virtual Xbox source and turn that switch off, then check that
touchpad/Back still works in the game without taking screenshots. Please also
check that the native Sony Share button still takes screenshots and saves replay
clips, including tap, hold and reconnecting the controller.

We tested the routes in automated regression tests, but real DS4Windows hardware
confirmation is still pending. Please tell us your DS4Windows version, output
controller mode, which source you selected, and whether those checks work. If
changing a binding affects both sources, please include their selected preset
names or a binding export so we can check shared assignments. There is no need
to change HidHide or disable XInput for this fix.

## GameSir G7 Pro

Thanks for reporting the dedicated Capture-button issue. I have an instrumented
portable beta ready, but the GameSir G7 Pro is still **Unverified** and I do not
want to guess its mapping. Please extract the beta, open Settings > Input, press
the dedicated button during the three-second Controller Probe, then click
**Copy controller compatibility report**. Please also test single tap, hold,
double/triple tap, a two-button combination and receiver reconnect. Send the
report with your firmware, connection mode and polling rate; it contains no
serial number or full PnP path.

## 8BitDo Ultimate 2

Thanks for helping verify the modern controller backend. The 8BitDo Ultimate 2
is still **Unverified** until its actual modes are tested. Please run the
portable beta over USB and 2.4 GHz in each available XInput/DInput mode. For
each mode, press the dedicated button in the binding editor and Controller
Probe, copy the compatibility report, and verify tap, hold, double/triple tap,
combination, sleep and reconnect. Include firmware and Windows version; GameHQ
will not infer a mapping from the model name alone.
