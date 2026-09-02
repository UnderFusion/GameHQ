import QtQuick

// The default skin: near-black cool glass lit from the edges by a soft blue
// bloom, pure white type, and a lot of air. Minimal and clean — no texture,
// nothing drawn that does not carry meaning. Console-dashboard in feel, which
// is what this app is for.
//
// The blues are deliberate rather than sampled: a deep cobalt (#003791) ramping
// into a brighter blue (#0070D1), with a teal edge bloom. The semantic accents
// are borrowed from the same family — a green for success, a magenta-pink for
// danger — chosen to read at a glance against near-black.
//
// Type is Segoe UI: humanist, and present on every Win10/11 box, so it never
// falls back to something unintended.
Skin {
    //% "Obsidian"
    label: qsTrId("gamehq.settings.theme.obsidian.label")
    //% "Near-black glass lit by a cool blue bloom. The default."
    blurb: qsTrId("gamehq.settings.theme.obsidian.description")

    bg0:        "#08090C"
    bg1:        "#101318"
    surface:    "#161A20"
    surfaceAlt: "#1F242B"
    stroke:     Qt.rgba(1, 1, 1, 0.09)
    borderLight: Qt.rgba(1, 1, 1, 0.28)
    // Control-center panels read as translucent smoked glass over the bloom.
    panelTint:  Qt.rgba(0.063, 0.075, 0.094, 0.88)

    // Gradient runs wordmark cobalt → X Blue, the console's own blue ramp.
    accent1: "#003791"
    accent2: "#0070D1"
    accent:  "#0070D1"

    text:      "#FFFFFF"
    textMuted: "#9BA1AB"
    textFaint: "#61676F"
    textOnAccent: "#FFFFFF"

    danger:  "#E83287"   // Circle Pink — circle is cancel/back on PlayStation
    success: "#1FAA8C"   // Triangle Green
    // No brand hue reads as a warning (Square Purple would not), so this stays
    // a plain amber: semantics beat fidelity when the color has to warn.
    warning: "#FFB020"

    scrim:         Qt.rgba(0.031, 0.035, 0.047, 0.66)
    lightboxScrim: Qt.rgba(0.031, 0.035, 0.047, 0.80)
    focusGlow: Qt.rgba(0, 0.439, 0.820, 0.45)
    hoverTint: Qt.rgba(1, 1, 1, 0.04)

    // Softly rounded cards, in the console's register — not machined, not pill.
    radiusS: 8
    radiusM: 12
    radiusL: 18

    // Fluid rather than snappy, with a pronounced focus pop: the PS5 grid lifts
    // the tile you are on.
    durFast:   130
    durNormal: 210
    durSlow:   320
    focusScale: 1.06

    // The home screen is near-black, lit from the edges by a faint cool bloom —
    // atmosphere you notice only once it is gone. Keep glowStrength low: on the
    // console this is a suggestion of light, not a colored wash, and a purple
    // orb (Square Purple) was simply wrong, however on-brand the hex is.
    backdropStyle: "wash"
    backdropTop:    "#0F1318"
    backdropBottom: "#040507"
    washA: "#0070D1"   // X Blue
    washB: "#1C6E8C"   // cool teal, matching the console's edge bloom
    glowStrength: 0.2

    // The PS5 UI is flat and clean — any grain or hatch would be a lie.
    texture: "none"

    dangerQuietBorder:  Qt.rgba(0.910, 0.196, 0.529, 0.45)
    dangerQuietTop:     "#3A1B2B"
    dangerQuietBottom:  "#28131E"
    successQuietBorder: Qt.rgba(0.122, 0.667, 0.549, 0.45)
    successQuietTop:    "#14332C"
    successQuietBottom: "#0E2521"

    fontFamily: "Segoe UI"
}
