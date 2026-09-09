pragma Singleton
import QtQuick
import "themes"

// Single source of truth for all visual values — see docs/design-system.md.
// Components must never hardcode colors, sizes, or durations.
//
// Colors resolve through the active skin (themes/Skin.qml and its variants);
// everything below the color block is fixed across skins because it is layout
// and behavior, not palette. Components are unaffected either way: every token
// name here is unchanged, so nothing outside this file knows skins exist.
QtObject {
    id: theme

    // ───────────────────────── Skin selection ─────────────────────────
    readonly property Skin darkSkin: DarkSkin {}
    readonly property Skin lightSkin: LightSkin {}
    readonly property Skin highContrastSkin: HighContrastSkin {}
    readonly property Skin midnightSkin: MidnightSkin {}
    readonly property Skin emeraldSkin: EmeraldSkin {}
    readonly property Skin harborSkin: HarborSkin {}
    readonly property Skin carbonSkin: CarbonSkin {}
    readonly property Skin cobaltSkin: CobaltSkin {}
    readonly property Skin synthwaveSkin: SynthwaveSkin {}
    readonly property Skin nordSkin: NordSkin {}
    readonly property Skin draculaSkin: DraculaSkin {}
    readonly property Skin gruvboxSkin: GruvboxSkin {}
    readonly property Skin obsidianSkin: ObsidianSkin {}

    // config key → skin. Adding a skin means one entry here and one in
    // skinOrder; the Settings picker builds itself from those.
    readonly property var skins: ({
        "dark":          darkSkin,
        "light":         lightSkin,
        "high_contrast": highContrastSkin,
        "midnight":      midnightSkin,
        "emerald":       emeraldSkin,
        "harbor":        harborSkin,
        "carbon":        carbonSkin,
        "cobalt":        cobaltSkin,
        "synthwave":     synthwaveSkin,
        "nord":          nordSkin,
        "dracula":       draculaSkin,
        "gruvbox":       gruvboxSkin,
        "obsidian":      obsidianSkin
    })

    // Presentation order in Settings: the default first, then the neutrals,
    // then the characterful ones.
    readonly property var skinOrder: [
        "obsidian", "dark", "light", "high_contrast",
        "midnight", "emerald", "harbor", "carbon", "cobalt",
        "synthwave", "nord", "dracula", "gruvbox"
    ]

    readonly property var availableSkins: theme.skinOrder.map(function (key) {
        return { key: key, label: theme.skins[key].label, blurb: theme.skins[key].blurb }
    })

    // Persisted as `theme.active_skin`. Assigned (not bound) so the Settings
    // combo can flip it live; seeded from config once the engine is up.
    property string activeSkin: "obsidian"

    // Unknown key → the default skin, so a hand-edited config.json cannot leave
    // the app with no palette at all.
    readonly property Skin skin: theme.skins[theme.activeSkin] || obsidianSkin

    // Persisted as `theme.overlay_scrim_strength` (percent, 25–150). A
    // multiplier on the active skin's scrim alpha rather than an absolute
    // alpha, so 100 always means "the skin's own dimming" on every skin.
    property int overlayScrimStrength: 100

    Component.onCompleted: {
        if (typeof app !== "undefined" && app) {
            theme.activeSkin = app.config("theme.active_skin", "obsidian")
            theme.overlayScrimStrength = app.config("theme.overlay_scrim_strength", 100)
        }
    }

    // Repaint live when the skin is changed from Settings — every color token
    // below binds to `skin`, so reassigning it re-evaluates all of them.
    readonly property Connections _configWatch: Connections {
        target: (typeof app !== "undefined") ? app : null
        function onConfigChanged(key, value) {
            if (key === "theme.active_skin")
                theme.activeSkin = value
            else if (key === "theme.overlay_scrim_strength")
                theme.overlayScrimStrength = Number(value)
        }
        function onConfigGroupReset(prefix) {
            if (prefix.length === 0 || prefix === "theme") {
                theme.activeSkin = app.config("theme.active_skin", "obsidian")
                theme.overlayScrimStrength = app.config("theme.overlay_scrim_strength", 100)
            }
        }
    }

    // ───────────────────────── Color tokens ─────────────────────────
    // Skinnable. Defaults and per-skin overrides live in themes/.
    readonly property color bg0:        skin.bg0
    readonly property color bg1:        skin.bg1
    readonly property color surface:    skin.surface
    readonly property color surfaceAlt: skin.surfaceAlt
    readonly property color stroke:     skin.stroke
    readonly property color borderLight: skin.borderLight   // visible light outline (e.g. capture previews)
    readonly property color accent1:    skin.accent1
    readonly property color accent2:    skin.accent2
    readonly property color accent:     skin.accent
    readonly property color text:       skin.text
    readonly property color textMuted:  skin.textMuted
    readonly property color textFaint:  skin.textFaint
    readonly property color danger:     skin.danger
    readonly property color success:    skin.success
    readonly property color warning:    skin.warning
    readonly property color scrim:      skin.scrim
    readonly property color lightboxScrim: skin.lightboxScrim
    // The in-game overlay's backdrop: the skin's scrim with its alpha scaled
    // by the user's Settings → Appearance dimming strength, capped just short
    // of opaque so the game behind the overlay never disappears entirely.
    readonly property color overlayScrim: Qt.rgba(skin.scrim.r, skin.scrim.g, skin.scrim.b,
                                                  Math.min(0.95, skin.scrim.a * theme.overlayScrimStrength / 100))
    readonly property color focusGlow:  skin.focusGlow
    // Translucent surface for in-overlay panels (sidebar, strip, preview frame)
    // — sits over the scrim with a hairline stroke so panels read as distinct
    // floating layers, not painted on the backdrop.
    readonly property color panelTint:  skin.panelTint

    // Foreground on the accent gradient — its own token rather than a `text`
    // variant, since it tracks the accent, not the body text. NOT named
    // `onAccent`: QML would parse that as a signal handler for the `accent`
    // property above and refuse to load the singleton.
    readonly property color textOnAccent: skin.textOnAccent
    // Wash laid over a control for hover/press feedback; the opacity is the
    // caller's, the color is the skin's (white on dark, black on light).
    readonly property color highlight:  skin.highlight
    // Faint hover fill for list rows that have no surface of their own.
    readonly property color hoverTint:  skin.hoverTint

    // Semantic aliases used by the settings card system. They deliberately
    // derive from every skin's existing palette so the redesign adds no
    // layout-changing or hard-coded theme branch.
    readonly property color surfaceElevated: skin.surfaceAlt
    readonly property color surfaceHover: skin.hoverTint
    readonly property color accentSoft: Qt.rgba(skin.accent.r, skin.accent.g, skin.accent.b, 0.14)
    readonly property color warningSoft: Qt.rgba(skin.warning.r, skin.warning.g, skin.warning.b, 0.10)
    readonly property color dangerSoft: Qt.rgba(skin.danger.r, skin.danger.g, skin.danger.b, 0.10)
    readonly property color successSoft: Qt.rgba(skin.success.r, skin.success.g, skin.success.b, 0.10)
    readonly property color focusRing: skin.accent
    readonly property color divider: skin.stroke

    // Video play badge — the circular ▶ marker drawn over a video thumbnail
    // (capture tile, overlay preview, toast). Reads on any frame, so it is
    // deliberately independent of the surface palette.
    readonly property color badgeFill:   skin.badgeFill
    readonly property color badgeBorder: skin.badgeBorder
    readonly property color badgeGlyph:  skin.badgeGlyph

    // Circular icon button floating over a thumbnail — darker than the badge
    // so its glyph stays legible without a border.
    readonly property color tileButtonIdle:  skin.tileButtonIdle
    readonly property color tileButtonHover: skin.tileButtonHover

    // Player's centered play/pause pulse.
    readonly property color pulseFill: skin.pulseFill

    // Quiet (low-emphasis) AccentButton tinted to a semantic color: a muted
    // gradient pair plus a resting border derived from the same hue. Only the
    // danger and success variants exist because only those are used.
    readonly property color dangerQuietBorder:  skin.dangerQuietBorder
    readonly property color dangerQuietTop:     skin.dangerQuietTop
    readonly property color dangerQuietBottom:  skin.dangerQuietBottom
    readonly property color successQuietBorder: skin.successQuietBorder
    readonly property color successQuietTop:    skin.successQuietTop
    readonly property color successQuietBottom: skin.successQuietBottom

    // ───────────────────────── Style tokens ─────────────────────────
    // Skinnable, like the colors: these carry a skin's character as much as its
    // palette does — machined vs pill-soft, snappy vs cinematic.
    readonly property string fontFamily: skin.fontFamily
    readonly property real letterSpacingWide: skin.letterSpacingWide

    readonly property int radiusS: skin.radiusS
    readonly property int radiusM: skin.radiusM
    readonly property int radiusL: skin.radiusL
    readonly property int radiusPill: 999   // a pill is a pill in every skin
    readonly property int borderWidth: skin.borderWidth

    readonly property int toastVisibleLimit: 4
    readonly property int toastLifespan: 3600
    readonly property int toastPendingLifespan: 60000

    readonly property int durFast:   skin.durFast
    readonly property int durNormal: skin.durNormal
    readonly property int durSlow:   skin.durSlow
    readonly property real focusScale: skin.focusScale

    // Backdrop treatment — consumed by components/ThemeBackdrop.qml.
    readonly property string backdropStyle: skin.backdropStyle
    readonly property color backdropTop:    skin.backdropTop
    readonly property color backdropBottom: skin.backdropBottom
    readonly property color washA: skin.washA
    readonly property color washB: skin.washB
    readonly property real glowStrength: skin.glowStrength

    readonly property string texture: skin.texture
    readonly property real textureOpacity: skin.textureOpacity
    readonly property color textureColor: skin.textureColor

    // ── Everything below is fixed across skins: layout, not style. ──
    // Sizes stay put because a skin must restyle the app, not re-lay-it-out.

    // Typography sizes
    readonly property int fontHero:    48   // oversized glyph in empty states
    readonly property int fontDisplay: 32   // weight Light
    readonly property int fontTitle:   22   // weight DemiBold
    readonly property int fontH3:      16   // weight DemiBold
    readonly property int fontBody:    14
    readonly property int fontCaption: 12

    // Spacing (4-base scale)
    readonly property int s4:  4
    readonly property int s8:  8
    readonly property int s12: 12
    readonly property int s16: 16
    readonly property int s24: 24
    readonly property int s32: 32
    readonly property int s48: 48

    // Modal dialogs
    readonly property int dialogWidth: 440

    // Video player controls
    readonly property int playerButtonSize: 44
    readonly property int playerControlsMaxWidth: 9999
    readonly property int playerSeekStepMs: 2000
    readonly property int playerHudHoldMs: 1600
    readonly property int playerControlsAutoHideMs: 5000
}
