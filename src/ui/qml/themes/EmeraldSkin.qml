// Fluent-flavored: charcoal surfaces, one confident green, rounded tiles and
// short snappy transitions. Where Midnight glides, this one responds.
Skin {
    //% "Emerald"
    label: qsTrId("gamehq.settings.theme.emerald.label")
    //% "Charcoal and green, with quick Fluent-style responses."
    blurb: qsTrId("gamehq.settings.theme.emerald.description")

    bg0:        "#0E0E0E"
    bg1:        "#141414"
    surface:    "#1A1A1A"
    surfaceAlt: "#232323"
    stroke:     Qt.rgba(1, 1, 1, 0.08)
    borderLight: Qt.rgba(1, 1, 1, 0.24)
    panelTint:  Qt.rgba(0.055, 0.055, 0.055, 0.88)

    accent1: "#9BF00B"
    accent2: "#107C10"
    accent:  "#3FA324"

    text:      "#E6E6E6"
    textMuted: "#9A9A9A"
    textFaint: "#6B6B6B"
    textOnAccent: "#0A0A0A"

    danger:  "#E74856"
    success: "#9BF00B"
    warning: "#FFB900"

    focusGlow: Qt.rgba(0.61, 0.94, 0.04, 0.42)
    hoverTint: Qt.rgba(1, 1, 1, 0.05)

    radiusS: 8
    radiusM: 12
    radiusL: 16

    // Fluent's tile feedback is short and immediate.
    durFast:   110
    durNormal: 170
    durSlow:   240
    focusScale: 1.05

    backdropStyle: "gradient"
    backdropTop:    "#161A16"
    backdropBottom: "#0A0A0A"
    glowStrength: 0.4

    successQuietBorder: Qt.rgba(0.61, 0.94, 0.04, 0.45)
    successQuietTop:    "#1E2A10"
    successQuietBottom: "#151E0B"
}
