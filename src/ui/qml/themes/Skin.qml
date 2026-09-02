import QtQuick

// Base for every skin: declares the full skinnable surface with the Blue values
// as defaults. A skin overrides only what it changes, so a token a skin forgets
// falls back to Blue rather than resolving to an invalid value.
//
// WHAT IS SKINNABLE: style — color, font family, roundness, border weight,
// motion, glow, backdrop treatment.
// WHAT IS NOT: layout — spacing steps, font sizes, control sizes. A skin
// restyles the app; it must not re-lay-it-out. Those stay in Theme.qml.
//
// Adding a token: declare it here first, then bind it in Theme.qml. Never name
// one `on<Existing>` (e.g. `onAccent` beside `accent`) — QML parses that as a
// signal handler and refuses to load the singleton, taking the whole UI down.
// See docs/design-system.md §0.
QtObject {
    // ── Identity ──────────────────────────────────────────────────
    //% "Blue"
    property string label: qsTrId("gamehq.settings.theme.blue.label")
    // One line, shown under the Settings picker.
    //% "Deep navy surfaces with violet-to-blue accents."
    property string blurb: qsTrId("gamehq.settings.theme.blue.description")

    // ── Surfaces ──────────────────────────────────────────────────
    property color bg0:        "#0B0F20"
    property color bg1:        "#131A33"
    property color surface:    "#171F3D"
    property color surfaceAlt: "#1D2749"
    property color stroke:     Qt.rgba(1, 1, 1, 0.06)
    property color borderLight: Qt.rgba(1, 1, 1, 0.22)
    property color panelTint:  Qt.rgba(0.0902, 0.1216, 0.2392, 0.82)

    // ── Brand accents ─────────────────────────────────────────────
    property color accent1: "#8B6BFF"
    property color accent2: "#3FA9FF"
    property color accent:  "#6D8DFF"

    // ── Text ──────────────────────────────────────────────────────
    property color text:      "#E8EAF0"
    property color textMuted: "#8B93A7"
    property color textFaint: "#5A6378"
    property color textOnAccent: "#FFFFFF"

    // ── Semantic ──────────────────────────────────────────────────
    property color danger:  "#FF5D73"
    property color success: "#4ADE80"
    property color warning: "#FFC24D"

    // Scrims sit behind media (lightbox, overlay) and stay dark in every skin —
    // a light scrim would wash out the capture it is meant to frame.
    property color scrim:         Qt.rgba(0.043, 0.059, 0.125, 0.6)
    property color lightboxScrim: Qt.rgba(0.043, 0.059, 0.125, 0.74)
    property color focusGlow:     Qt.rgba(0.427, 0.553, 1, 0.35)

    // Hover/press wash. `highlight` is the color, the opacity is the caller's —
    // so a light skin flips this to black rather than changing every call site.
    property color highlight: "#FFFFFF"
    property color hoverTint: Qt.rgba(1, 1, 1, 0.03)

    // Chrome drawn over video frames (play badge, tile buttons, player pulse)
    // must read on arbitrary imagery, so it stays dark in every skin.
    property color badgeFill:   Qt.rgba(0, 0, 0, 0.40)
    property color badgeBorder: Qt.rgba(1, 1, 1, 0.9)
    property color badgeGlyph:  Qt.rgba(1, 1, 1, 0.95)
    property color tileButtonIdle:  Qt.rgba(0, 0, 0, 0.55)
    property color tileButtonHover: Qt.rgba(0, 0, 0, 0.8)
    property color pulseFill: Qt.rgba(0, 0, 0, 0.46)

    // ── Quiet AccentButton variants ───────────────────────────────
    property color dangerQuietBorder:  Qt.rgba(1.0, 0.36, 0.45, 0.45)
    property color dangerQuietTop:     "#301B28"
    property color dangerQuietBottom:  "#21141E"
    property color successQuietBorder: Qt.rgba(0.29, 0.87, 0.50, 0.45)
    property color successQuietTop:    "#173328"
    property color successQuietBottom: "#11251E"

    // ── Typography ────────────────────────────────────────────────
    // Family only. Sizes are layout and stay fixed. A missing family falls back
    // through Qt's font matching, so naming one that is not installed degrades
    // to the system default rather than failing.
    property string fontFamily: "Segoe UI Variable Display"
    property real letterSpacingWide: 1

    // ── Shape ─────────────────────────────────────────────────────
    // Roundness is a skin's loudest signal after color: pill-soft vs machined.
    property int radiusS: 8
    property int radiusM: 12
    property int radiusL: 16
    // Hairline by default; skins that want drawn edges raise it.
    property int borderWidth: 1

    // ── Motion ────────────────────────────────────────────────────
    // "Reactive" lives here: shorter durations and a bigger focus pop read as
    // snappier without touching a single component.
    property int durFast:   140
    property int durNormal: 220
    property int durSlow:   320
    property real focusScale: 1.04

    // ── Backdrop ──────────────────────────────────────────────────
    // How the desktop window paints behind its content:
    //   flat      — single bg0 fill (the original look)
    //   gradient  — linear backdropTop → backdropBottom
    //   wash      — gradient plus soft blurred accent orbs (console-dashboard feel)
    //   scanlines — gradient plus fine horizontal rules
    property string backdropStyle: "flat"
    property color backdropTop:    "#0B0F20"
    property color backdropBottom: "#0B0F20"
    // Orb tints for `wash`.
    property color washA: "#8B6BFF"
    property color washB: "#3FA9FF"
    // 0 = none. Scales the focus bloom and the wash orb opacity.
    property real glowStrength: 0

    // ── Texture ───────────────────────────────────────────────────
    // An optional fine pattern over the backdrop, generated procedurally and
    // tiled — never a bundled image. That keeps it pixel-exact at any window
    // size and DPI, tinted from the skin's own palette, and free of both
    // licensing and package weight.
    //   none  — no overlay
    //   grain — sparse random specks (film/paper)
    //   hatch — fine diagonal rules
    //   grid  — thin blueprint lattice
    //   weave — over-under twill (carbon fiber)
    property string texture: "none"
    // Keep this low. Texture is atmosphere; the moment it is legible as a
    // pattern it competes with the captures, which are the actual content.
    property real textureOpacity: 0.04
    property color textureColor: "#FFFFFF"
}
