# GameHQ 0.7.8 local acceptance package

This is a local candidate, not a published release. VERSION 0.7.8 supersedes
unpublished per-slice labels 0.7.8 through 0.7.43; published history through
0.7.7 remains intact. No version bump is required for an internal slice.

## Identity and verification

The adjacent beta-manifest.json records the exact source commit, working-tree
state, executable hashes and ZIP SHA-256. Verify
GameHQ-0.7.8-feedback-wave-beta-portable.zip against its .sha256 sidecar.
Extract into a fresh folder and run GameHQ.exe; retain portable.flag.

## Requested test

Only the DSX switching reproduction in dsx-switching-0.7.8.md is requested.
The owner's passing native wired DualSense observations are retained there;
no repeat of those tests is required for this optional-callback correction.
The abnormal-kill test was skipped by the owner with residual risk accepted,
not passed. Unreported preset/capture and named-title checklist cases remain open.

This package includes the approved hint bubble and sidebar highlight fixes,
version consolidation and provider lifecycle diagnostics. Optional GameInput
Guide/Share registration failure now retains ordinary readings and focus policy,
while leaving system buttons to existing eligible providers. Foreground
acquisition, exclusive-policy masks and neutral-handoff ordering are unchanged.

## Package probes

Validation uses --assert-version 0.7.8, --release-trust-self-test,
--localization-assets-self-test en, --localization-assets-self-test de,
and --smoke-test on a fresh extraction. Results accompany the package.
Existing packaging deviations remain listed in beta-manifest.json.
