#include "input/DefaultBindings.h"

// Built-in defaults are written in the pattern model: `tap(n)` carries its own
// count, and a `hold()` with no duration stores 0, which means "use whatever
// hold duration is configured". Baking the configured value into every default
// row instead would give the same setting two homes.
QVector<BindingResolver::Binding> gamehqDefaultBindings()
{
    using namespace ControlId;
    const auto c = [](const char* action, int slot, const QString& trigger,
                      const GestureSpec& gesture = GestureSpec::press()) {
        return BindingResolver::Binding{QStringLiteral("controller"), {},
                                        QString::fromLatin1(action), slot, trigger,
                                        gesture.activationCode(), gesture.holdMs, false,
                                        gesture.tapCount};
    };
    const auto k = [](const char* action, int slot, const char* chord) {
        return BindingResolver::Binding{QStringLiteral("keyboard"), {},
                                        QString::fromLatin1(action), slot,
                                        QString::fromLatin1(chord), QStringLiteral("press"),
                                        0, false, 1};
    };
    const auto tap = [](int count) { return GestureSpec::tap(count); };
    // No argument = the configured default hold duration.
    const auto hold = [](int ms = 0) { return GestureSpec::hold(ms); };

    return {
        k("global.toggle_overlay", 1, "Ctrl+Shift+G"),
        k("global.screenshot", 1, "Ctrl+Shift+S"),
        k("global.save_replay", 1, "Ctrl+Shift+E"),

        k("overlay.navigate_up", 1, "Up"),
        k("overlay.navigate_up", 2, "W"),
        k("overlay.navigate_down", 1, "Down"),
        k("overlay.navigate_down", 2, "S"),
        k("overlay.navigate_left", 1, "Left"),
        k("overlay.navigate_left", 2, "A"),
        k("overlay.navigate_right", 1, "Right"),
        k("overlay.navigate_right", 2, "D"),
        k("overlay.confirm", 1, "Return"),
        k("overlay.confirm", 2, "Enter"),
        k("overlay.back", 1, "Esc"),
        k("overlay.back", 2, "Backspace"),
        k("overlay.favorite", 1, "F"),
        k("overlay.menu", 1, "M"),
        k("overlay.sidebar_toggle", 1, "Tab"),
        k("overlay.game_prev", 1, "PgUp"),
        k("overlay.game_next", 1, "PgDown"),

        k("desktop.navigate_up", 1, "Up"),
        k("desktop.navigate_up", 2, "W"),
        k("desktop.navigate_down", 1, "Down"),
        k("desktop.navigate_down", 2, "S"),
        k("desktop.navigate_left", 1, "Left"),
        k("desktop.navigate_left", 2, "A"),
        k("desktop.navigate_right", 1, "Right"),
        k("desktop.navigate_right", 2, "D"),
        k("desktop.confirm", 1, "Return"),
        k("desktop.confirm", 2, "Enter"),
        k("desktop.back", 1, "Esc"),
        k("desktop.back", 2, "Backspace"),
        k("desktop.favorite", 1, "F"),
        k("desktop.menu", 1, "M"),
        k("desktop.tab_prev", 1, "PgUp"),
        k("desktop.tab_next", 1, "PgDown"),

        k("playback.play_pause", 1, "Space"),
        k("playback.play_pause", 2, "Return"),
        k("playback.seek_back", 1, "Left"),
        k("playback.seek_forward", 1, "Right"),
        // S grabs the on-screen clip frame while playback is focused. Only
        // resolves in Playback scope (nothing else binds a bare S there), so it
        // never collides with the global Ctrl+Shift+S screenshot hotkey.
        k("playback.frame_grab", 1, "S"),

        c("global.screenshot", 1, Capture, tap(1)),
        c("global.save_replay", 1, Capture, hold()),
        // Guide carries a tap/hold pair like Share's screenshot/save-replay:
        // a tap toggles the overlay, a 2 s hold summons the desktop window
        // with focus (hold again to return to the game). Tap, not press, so
        // the hold cannot also fire the overlay on the way down — the runtime
        // only suppresses a tap when a hold on the same control has fired.
        c("global.toggle_overlay", 1, Guide, tap(1)),
        c("global.toggle_overlay", 2, Capture, tap(2)),
        c("global.toggle_desktop", 1, Guide, hold(2000)),

        c("overlay.navigate_up", 1, DpadUp),
        c("overlay.navigate_down", 1, DpadDown),
        c("overlay.navigate_left", 1, DpadLeft),
        c("overlay.navigate_right", 1, DpadRight),
        c("overlay.confirm", 1, FaceSouth),
        c("overlay.back", 1, FaceEast),
        c("overlay.favorite", 1, FaceNorth),
        c("overlay.menu", 1, FaceWest),
        c("overlay.sidebar_toggle", 1, Menu),
        c("overlay.game_prev", 1, ShoulderLeft),
        c("overlay.game_next", 1, ShoulderRight),

        c("desktop.navigate_up", 1, DpadUp),
        c("desktop.navigate_down", 1, DpadDown),
        c("desktop.navigate_left", 1, DpadLeft),
        c("desktop.navigate_right", 1, DpadRight),
        // Cross is tap, not press: it shares the button with the bulk-select
        // hold below, and the runtime only suppresses a tap when a hold has
        // already fired. On "press" both would fire on a long press — opening
        // the capture *and* entering bulk mode. Same shape as Share's
        // screenshot-tap / save-replay-hold pair above.
        c("desktop.confirm", 1, FaceSouth, tap(1)),
        c("desktop.back", 1, FaceEast),
        c("desktop.favorite", 1, FaceNorth),
        c("desktop.menu", 1, FaceWest),
        c("desktop.tab_prev", 1, ShoulderLeft),
        c("desktop.tab_next", 1, ShoulderRight),
        // Options opens Settings: in Desktop scope that button was unbound
        // (its only binding is overlay.sidebar_toggle, a different scope).
        c("desktop.settings", 1, Menu),
        c("desktop.zoom_out", 1, TriggerLeft),
        c("desktop.zoom_in", 1, TriggerRight),
        // The right stick scrolls whichever view is showing, like a mouse
        // wheel — the selection stays put. Decoded by the Sony and XInput
        // backends (WinMM has no reliable right-stick axis mapping).
        c("desktop.scroll_up", 1, StickRightUp),
        c("desktop.scroll_down", 1, StickRightDown),
        // Hold Cross for a second to enter bulk selection. Cross keeps its
        // press binding (desktop.confirm) — the runtime only fires the hold
        // once the button has been down that long, so a normal tap still opens
        // the capture.
        c("desktop.bulk_toggle", 1, FaceSouth, hold(1000)),

        c("playback.play_pause", 1, FaceSouth),
        c("playback.seek_back", 1, DpadLeft),
        c("playback.seek_forward", 1, DpadRight),
        // Share (Create) grabs the current clip frame while playback is focused.
        // In every other scope Share tap is global.screenshot. The two do not
        // stack: ContextOverrideCatalog declares frame_grab as an explicit
        // substitution for screenshot on the tap gesture, so a tap during
        // playback produces one capture, not two. Hold (save_replay) and
        // double-tap (toggle_overlay) are different activations and stay live.
        c("playback.frame_grab", 1, Capture, tap(1)),
    };
}
