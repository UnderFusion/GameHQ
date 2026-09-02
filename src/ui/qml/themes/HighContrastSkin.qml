// Maximum legibility: near-black surfaces, pure-white text, and borders that
// are actually visible rather than hairlines. Accents shift to the brightest
// members of the same hues so they still carry meaning at this contrast.
//
// The muted text steps are pulled much closer to full white on purpose — the
// dark skin's textFaint is decorative, and decorative is exactly what this skin
// is not for.
Skin {
    //% "High contrast"
    label: qsTrId("gamehq.settings.theme.high_contrast.label")

    bg0:        "#000000"
    bg1:        "#0A0A0A"
    surface:    "#141414"
    surfaceAlt: "#1F1F1F"
    // Hairlines defeat the purpose here — these are meant to be seen.
    stroke:     Qt.rgba(1, 1, 1, 0.35)
    borderLight: Qt.rgba(1, 1, 1, 0.75)
    panelTint:  Qt.rgba(0, 0, 0, 0.92)

    accent1: "#B79CFF"
    accent2: "#66C6FF"
    accent:  "#8FA8FF"

    text:      "#FFFFFF"
    textMuted: "#D6D9E0"
    textFaint: "#A8ADBA"
    textOnAccent: "#000000"

    danger:  "#FF7A8C"
    success: "#5CF29A"
    warning: "#FFD24D"

    scrim:         Qt.rgba(0, 0, 0, 0.78)
    lightboxScrim: Qt.rgba(0, 0, 0, 0.9)
    focusGlow:     Qt.rgba(0.56, 0.66, 1, 0.7)

    hoverTint: Qt.rgba(1, 1, 1, 0.12)

    badgeBorder: Qt.rgba(1, 1, 1, 1)
    badgeGlyph:  Qt.rgba(1, 1, 1, 1)
    badgeFill:   Qt.rgba(0, 0, 0, 0.7)
    tileButtonIdle:  Qt.rgba(0, 0, 0, 0.75)
    tileButtonHover: Qt.rgba(0, 0, 0, 0.92)

    dangerQuietBorder:  Qt.rgba(1.0, 0.48, 0.55, 0.85)
    dangerQuietTop:     "#3D1620"
    dangerQuietBottom:  "#2A0F17"
    successQuietBorder: Qt.rgba(0.36, 0.95, 0.60, 0.85)
    successQuietTop:    "#0F3A24"
    successQuietBottom: "#0A2818"
}
