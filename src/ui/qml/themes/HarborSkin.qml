// The blue-grey library look: flat, matte, squared-off, utilitarian. No glow,
// almost no rounding — chrome recedes so the capture art carries the screen.
// Condensed geometric type for legibility at couch distance.
Skin {
    //% "Harbor"
    label: qsTrId("gamehq.settings.theme.harbor.label")
    //% "Flat blue-gray with squared edges. Chrome gets out of the way."
    blurb: qsTrId("gamehq.settings.theme.harbor.description")

    bg0:        "#171A21"
    bg1:        "#1B2838"
    surface:    "#1B2838"
    surfaceAlt: "#2A475E"
    stroke:     Qt.rgba(1, 1, 1, 0.09)
    borderLight: Qt.rgba(0.78, 0.84, 0.88, 0.30)
    panelTint:  Qt.rgba(0.106, 0.157, 0.220, 0.90)

    accent1: "#66C0F4"
    accent2: "#417A9B"
    accent:  "#66C0F4"

    text:      "#C7D5E0"
    textMuted: "#8F98A0"
    textFaint: "#626A72"
    textOnAccent: "#0B1218"

    danger:  "#C15755"
    success: "#5BA32B"
    warning: "#E1B12C"

    focusGlow: Qt.rgba(0.4, 0.75, 0.96, 0.30)

    fontFamily: "Bahnschrift"

    // Machined, not soft.
    radiusS: 2
    radiusM: 3
    radiusL: 4

    // Measured — a highlight outline, not a flourish.
    durFast:   120
    durNormal: 180
    durSlow:   260
    focusScale: 1.02

    backdropStyle: "gradient"
    backdropTop:    "#1B2838"
    backdropBottom: "#12151B"
    glowStrength: 0.15

    // Faint hatch keeps the deliberately flat chrome from looking unfinished.
    texture: "hatch"
    textureOpacity: 0.022
    textureColor: "#C7D5E0"
}
