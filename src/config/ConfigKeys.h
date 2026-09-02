#pragma once
#include <QLatin1StringView>

// The complete set of config.json keys (docs/configuration.md). ConfigManager
// stores flat, dotted keys; these constants are the single place a key is
// spelled, so a typo is a compile error instead of a silent fall back to the
// default value.
//
// Scope: C++ only. QML reaches config through app.config("capture.mode") and
// keeps using string literals — QML has no access to these constants, and the
// property names are already part of that surface.
//
// Adding a key: declare it here and add its default to ConfigManager::defaults().
// The two lists are expected to match exactly.
namespace ConfigKeys
{
// capture.* — screenshot behavior and when capture is allowed at all.
inline constexpr QLatin1StringView CaptureMode{ "capture.mode" };                          // only_in_games | whitelist | always
inline constexpr QLatin1StringView CaptureScreenshotFormat{ "capture.screenshot_format" }; // png | jpg
inline constexpr QLatin1StringView CaptureJpegQuality{ "capture.jpeg_quality" };           // 1-100, only when format=jpg
inline constexpr QLatin1StringView CaptureScreenshotSound{ "capture.screenshot_sound" };
inline constexpr QLatin1StringView CaptureScreenshotNotify{ "capture.screenshot_notify" };

// replay.* — rolling buffer and saved-clip encoding.
inline constexpr QLatin1StringView ReplayClipSound{ "replay.clip_sound" };
inline constexpr QLatin1StringView ReplayClipNotify{ "replay.clip_notify" };
inline constexpr QLatin1StringView ReplayResolution{ "replay.resolution" };
inline constexpr QLatin1StringView ReplayFps{ "replay.fps" };
inline constexpr QLatin1StringView ReplayBitrateMbps{ "replay.bitrate_mbps" };
inline constexpr QLatin1StringView ReplayLengthSeconds{ "replay.length_seconds" };   // UI set {30,60,180,300,600,900}
inline constexpr QLatin1StringView ReplaySegmentSeconds{ "replay.segment_seconds" }; // ring granularity, not in the UI
inline constexpr QLatin1StringView ReplayAuto{ "replay.auto" };                      // auto-arm master switch

// input.* / audio.*
// Gesture timing. All three are read by the pattern recognizer; the editor
// shows the same numbers, so a notice like "waits up to 300 ms" is never a
// guess. InputShareHoldMs is the pre-0.7.3 name for the hold duration and is
// migrated into InputDefaultHoldMs once, override-only.
inline constexpr QLatin1StringView InputShareHoldMs{ "input.share_hold_ms" };      // LEGACY, read once
inline constexpr QLatin1StringView InputDefaultHoldMs{ "input.default_hold_ms" };
inline constexpr QLatin1StringView InputMultiTapIntervalMs{ "input.multi_tap_interval_ms" };
inline constexpr QLatin1StringView InputChordWindowMs{ "input.chord_window_ms" };
inline constexpr QLatin1StringView InputModernControllerSupport{ "input.modern_controller_support" }; // auto | off
inline constexpr QLatin1StringView AudioEnabled{ "audio.enabled" };

// storage.* — empty means "use the managed default root" (see CaptureLocations).
inline constexpr QLatin1StringView StorageScreenshotsRoot{ "storage.screenshots_root" };
inline constexpr QLatin1StringView StorageClipsRoot{ "storage.clips_root" };

// startup.* / tray.* / sounds.* / notifications.*
inline constexpr QLatin1StringView StartupEnabled{ "startup.enabled" };
inline constexpr QLatin1StringView StartupMinimized{ "startup.minimized" };
inline constexpr QLatin1StringView SoundsEnabled{ "sounds.enabled" };
inline constexpr QLatin1StringView SoundsVolume{ "sounds.volume" };
inline constexpr QLatin1StringView TrayCloseToTray{ "tray.close_to_tray" };
inline constexpr QLatin1StringView TrayMinimizeToTray{ "tray.minimize_to_tray" };
inline constexpr QLatin1StringView NotificationsEnabled{ "notifications.enabled" };

// ui.* — application presentation preferences. The explicit value "system"
// is retained so first-run bootstrap can distinguish it from an absent key.
inline constexpr QLatin1StringView UiLanguage{ "ui.language" };

// theme.* — appearance. Read by the QML Theme singleton, which resolves an
// unknown value back to Obsidian rather than leaving the app unpainted.
inline constexpr QLatin1StringView ThemeActiveSkin{ "theme.active_skin" };  // see Theme.skinOrder (QML); default obsidian
// Percent multiplier (25-150) on the active skin's overlay scrim alpha, so
// "100" always means "the skin's own dimming" regardless of which skin is on.
inline constexpr QLatin1StringView ThemeOverlayScrimStrength{ "theme.overlay_scrim_strength" };

// updates.* — automatic update-check policy (docs/updater.md "Discovery").
inline constexpr QLatin1StringView UpdatesCheckAutomatically{ "updates.check_automatically" };
inline constexpr QLatin1StringView UpdatesSkippedVersion{ "updates.skipped_version" };

// internal.* — safety metadata, never shown in the UI and deliberately absent
// from ConfigManager::defaults(). ConfigManager preserves internal.* across a
// reset-all, so these keys survive "Restore all defaults" by design.
inline constexpr QLatin1StringView InternalCaptureRootHistory{ "internal.capture_root_history" };
// UpdateService's cached ETag + the last completed check time, used to gate
// automatic checks to at most once every 24h (App::init()). last_seen_version
// and pending_post_update_version are reserved for the post-update greeting
// that lands with the safe updater helper (docs/updater.md stage 6/7).
inline constexpr QLatin1StringView InternalUpdatesEtag{ "internal.updates.etag" };
inline constexpr QLatin1StringView InternalUpdatesLastCheckUtc{ "internal.updates.last_check_utc" };
// Rate-limit cooldown. Persisted so restarting GameHQ is not a way to keep
// asking an exhausted budget.
inline constexpr QLatin1StringView InternalUpdatesNextAllowedCheckUtc{ "internal.updates.next_allowed_check_utc" };
inline constexpr QLatin1StringView InternalUpdatesLastSeenVersion{ "internal.updates.last_seen_version" };
inline constexpr QLatin1StringView InternalUpdatesPendingPostUpdateVersion{ "internal.updates.pending_post_update_version" };
// Version whose bundled What's New notes the user last closed. Unlike the
// updater greeting keys above, this also covers opening About manually.
inline constexpr QLatin1StringView InternalUiWhatsNewSeenVersion{ "internal.ui.whats_new_seen_version" };
// Hidden HDR capture gate (t24/t22), enabled by default and not exposed in
// Settings. Keeping the key allows an emergency per-installation fallback.
// Even when enabled, the SDR path is still used unless HdrCapabilities reports
// the capture target's display as HDR-active AND the FP16 tone-map stage
// initializes successfully — any failure falls back to the existing SDR
// pool format, so the default changes nothing on an SDR-only machine.
inline constexpr QLatin1StringView InternalCaptureExperimentalHdr{ "internal.capture.experimental_hdr" };
inline constexpr bool InternalCaptureExperimentalHdrDefault{ true };

// Group prefixes used by resetGroup() and the settings reset taxonomy.
namespace Group
{
inline constexpr QLatin1StringView Capture{ "capture" };
inline constexpr QLatin1StringView Replay{ "replay" };
inline constexpr QLatin1StringView Storage{ "storage" };
inline constexpr QLatin1StringView Startup{ "startup" };
inline constexpr QLatin1StringView Sounds{ "sounds" };
inline constexpr QLatin1StringView Tray{ "tray" };
inline constexpr QLatin1StringView Notifications{ "notifications" };
inline constexpr QLatin1StringView Ui{ "ui" };
inline constexpr QLatin1StringView Input{ "input" };
inline constexpr QLatin1StringView Audio{ "audio" };
inline constexpr QLatin1StringView Theme{ "theme" };
inline constexpr QLatin1StringView Updates{ "updates" };
} // namespace Group
} // namespace ConfigKeys
