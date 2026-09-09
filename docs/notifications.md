# Notifications (in-app toasts)

`NotificationCenter` owns a `ToastModel` for the visible stack. `ToastWindow.qml`
binds its Repeater to this model. Cards remain non-activating, click-through and
positioned at the bottom-right of the foreground monitor.

## Operations and ordinary notifications

`post(title, body, image, kind, when, isVideo)` creates an independent ordinary
notification. `post(CaptureRequest::id, ...)` creates a pending operation row;
`update(id, ...)` changes that same row to its terminal saved/failed state.
Operation keys are decimal strings in QML, preserving the full 64-bit identity.
The same request id travels through screenshot encode jobs, HDR continuation,
and admitted replay export; immediate replay rejections keep their own ids.

An identical post/update does not alter the row revision or restart its timer.
A duplicate receipt cannot turn a terminal row back into pending. Unknown,
expired, or evicted ids return false from update and never resurrect a toast.
Screenshot failure/skip updates an existing row to error with its reason, without
introducing a standalone failure notification; broader failure policy is p3-3.
Existing ordinary notification and capture sound/preferences remain in force.

## Visible stack and timing

Theme owns `toastVisibleLimit` (4), `toastLifespan` (3600 ms), and
`toastPendingLifespan` (60000 ms). Posting beyond the cap evicts the oldest visible
row. This is presentation only; it does not delete capture records or notification
history elsewhere. A terminal update restarts the short lifetime and cancels any
in-flight exit animation. Dismissals use a stable row key and revision, so a stale
fade completion cannot remove a newer state or another toast.

`clipSaved` and screenshot completion still commit media to the gallery before
updating the visible toast. Eviction/expiry does not affect saving, sounds, logs,
or gallery updates. A final result for an evicted row is not shown again.

## Validation

`tst_notificationcenter` checks row-preserving updates, large ids, duplicates,
unknown/evicted ids, stale dismissals, ordinary posts and a 20-post burst under a
configurable cap. `tst_captureacknowledgement` also verifies request ids on gated
outcomes and independent ids through overlapping screenshot encode jobs.

Acceptance receipt (2026-09-09): both focused executables passed 7/7. The isolated
App/QML harness exercised actual InputEngine signals and App handlers with capture
gates closed: keyboard/controller replay and screenshot requests retained the
same persistent model row through the terminal error update, with feedback
submission times 8/0/0/0 ms. Theme supplied cap=4; 20 further posts left 4 visible
rows. No QML assignment/type/reference errors were reported. Evidence:
`.claude-gui-temp/p3-2-validation/` and
`.claude-gui-temp/p3-2-app-receipt/gamehq-data/logs/gamehq.log`.
