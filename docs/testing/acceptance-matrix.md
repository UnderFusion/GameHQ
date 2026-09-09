# Acceptance Matrix — Feedback Reliability Wave (M02-M12)

Maps each acceptance criterion from the 2026-09-08 feedback-reliability wave to
the executable test (or tests) that prove it, plus any residual observation that
can only be made on real hardware. M01 (View/Back request-id trace) belongs to
the sibling input item and is deliberately excluded here.

Every automated case below is exercised by a committed ctest target. The full
gate is `tools/cmake/bin/ctest.exe --test-dir out --output-on-failure`.

Full-gate result recorded during this audit:
`out/acceptance-gate-M02-M12.log` — **100% tests passed, 0 failed out of 92**.

## M02 — two saves inside one second produce two files; the first is byte-identical afterwards

Automated: `tst_capturepublisher` (`twoClipSavesInsideOneSecondBothSurvive`,
`aFailedExportNeverDeletesAnExistingClip`) pin millisecond-*and*-collision-unique
names and prove a later save never touches an earlier clip's bytes.

No residual manual observation.

## M03 — game switch and buffer restart during export still yield a complete clip; shutdown never leaves a `.partial` file undetected

Automated: `tst_segmentrecorder` (`exportReadsCompleteSnapshotAfterRecorderReplacement`,
`replacementRecorderRetainsLeasedStalePaths`, `overlappingLeasesReleaseOnlyTheirOwnPaths`,
`oldLeaseDoesNotBlockUnrelatedTrimming`) and `tst_framepumpservice`
(`manualOwnershipSurvivesSettingsRestartAndStaleCallbacks`,
`staleWorkerFactsCannotOverwriteNewerRequests`). The lease/snapshot semantics that
survive a pipe replacement are already unit-locked.

No residual manual observation.

## M04 — apartment/session failure ends in `Failed` state, never `Recording`

Automated: `tst_framepumpservice` (`startupFailureIsTerminalForThatGeneration`,
`onlyConfirmedStartupReportsRecording`, `everyNotReadyStateRejectsSaves`).

No residual manual observation.

## M05 — manual-mode save survives `restartBuffer` and an HDR screenshot

Automated: `tst_framepumpservice` (`manualColdArmBecomesReadyAndSaves`,
`manualOwnershipSurvivesSettingsRestartAndStaleCallbacks`,
`hdrAndManualOwnersReleaseIndependently`, `saveFailureReleasesManualOwnershipButKeepsAuto`).

No residual manual observation.

## M06 — one save shows one toast transitioning Saving → Saved/Failed; a burst of 20 posts shows at most the configured cap

Automated: `tst_notificationcenter` (`updatesTheSameRowAndPreservesLargeIds`,
`duplicatePostsAndUpdatesDoNotRestartPresentation`, `twentyPostsStayWithinPresentationCap`,
`unknownAndEvictedUpdatesDoNotCreateToasts`).

No residual manual observation.

## M07 — screenshot skipped/failed shows a toast with the reason

Automated: `tst_captureacknowledgement` (`concurrentScreenshotEncodesRetainTheirOwnIds`,
`receiptPrecedesRejection`) plus `tst_notificationcenter` (`staleDismissalCannotRemoveUpdatedToast`
and the error-kind cases) cover the skip/fail reason plumbing.

No residual manual observation.

## M08 — forced DB insert failure shows "saved to disk, not in library"

Automated: `tst_capturecommitoutcome` (`rejectedRowWithFileOnDiskIsMediaOnly`,
`rejectedScreenshotRowIsMediaOnly`, `rejectedRowWithNoFileIsFailed`,
`alreadyIndexedFileIsNotReportedAsMissing`) drive a real aborting SQLite trigger,
not a mock, so the `MediaOnly` verdict survives internal changes.

No residual manual observation.

## M09 — border state is logged as Unknown/Unsupported/Denied/Hidden; hidden on Windows 11 after access is granted

Automated: `tst_captureborderstate` (`rawAccessIntegersMatchWindowsMetadata`,
`hiddenNeedsEveryCondition`, `windows10IsNeverHidden`, `deniedAccessStatuses`,
`readBackContradictionIsDenied`, `rawStatusFourIsAllowedAndCanBeHidden`, and the
`_data`-driven rows) pin the full state derivation and the raw ABI integers.

**Residual (manual-only → p7-5):** the *visible* yellow border actually disappearing
on the reporter's Windows 11 desktop. The automated test proves the state logic;
only a physical screen shows whether Windows honors the suppression.

## M10 — close on Settings → Controls with filter Favorites; reopen restores page, category and filter

Automated: `tst_navigationstate` (`pageAndCategoryRoundTrip`, `helpIsNeverRestored`,
`galleryFilterKeepsAGameThatStillExists`, `galleryFilterDropsAGameThatIsGone`,
`transientNavigationDoesNotPersist`, `overlayCategoryIsRememberedPerGame`,
`legacyCategoryIndexMigratesOnceAndIsIdempotent`).

No residual manual observation.

## M11 — window saved on a negative-coordinate monitor reopens there; disconnected monitor → centered on primary

Automated: `tst_windowplacement` (`restoresMonitorLeftOfPrimary`,
`restoresMonitorAbovePrimary`, `recentresRectangleFromDisconnectedMonitor`,
`recentresSliverThatCannotBeGrabbed`, `clampsOversizedWindowOnRecovery`,
`survivesWithoutScreens`).

**Residual (manual-only → p7-5):** physical multi-monitor confirmation — a real monitor
left of / above the primary, and a hot-unplug. The rectangle logic is fully covered;
only real displays confirm the OS-level restore path.

## M12 — capture sounds measure ≥ +6 dB RMS over `nav_tick`; every effect logs Ready or a real error

Automated: `tst_soundengine` (`onlyCaptureFeedbackUsesTheCaptureLevel`,
`theCaptureSliderIsReadPerceptually`, `theInterfaceSliderKeepsItsStraightMapping`,
`thePreviewPlaysTheRealCaptureSound`,
`anExistingConfigKeepsItsVolumeAndGainsTheCaptureDefault`, and the
`SoundLoadTracker` ready/failure cases). The objective amplitude check itself is an
asset-level measurement documented in `docs/sound-system.md`: RMS offsets of
+11.78 / +17.97 / +9.13 dB over `nav_tick` for the three capture assets, verified
against the generated WAV pack (regenerable via `assets/sounds/generate_sounds.py`).

Objectively measured amplitude: `docs/sound-system.md` table (capture assets all
≥ +6 dB RMS over `nav_tick`).

**Residual (manual-only):** perceptual audibility of the capture chime over a running
game. The RMS offset and per-level mapping are automated/measured; only a listener
confirms it reads as "audible but not jarring" amid game audio.
