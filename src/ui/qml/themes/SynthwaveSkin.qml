// The loud one: hot magenta and electric cyan over a violet-to-black horizon,
// scanlines across the backdrop, and bloom on everything focused. Sharp edges —
// neon reads as neon on hard corners, not soft ones.
Skin {
    //% "Synthwave"
    label: qsTrId("gamehq.settings.theme.synthwave.label")
    //% "Neon magenta and cyan over a violet horizon, with scanlines."
    blurb: qsTrId("gamehq.settings.theme.synthwave.description")

    bg0:        "#01012B"
    bg1:        "#0B0430"
    surface:    "#160A3A"
    surfaceAlt: "#221052"
    stroke:     Qt.rgba(1, 0.16, 0.43, 0.22)
    borderLight: Qt.rgba(0.02, 0.85, 0.91, 0.55)
    panelTint:  Qt.rgba(0.043, 0.016, 0.169, 0.88)

    accent1: "#FF2A6D"
    accent2: "#05D9E8"
    accent:  "#FF2A6D"

    text:      "#F7F2FF"
    textMuted: "#B39CD9"
    textFaint: "#7A63A8"
    textOnAccent: "#FFFFFF"

    danger:  "#FF2A6D"
    success: "#05FFA1"
    warning: "#FF9F1C"

    focusGlow: Qt.rgba(1, 0.16, 0.43, 0.75)
    highlight: "#05D9E8"
    hoverTint: Qt.rgba(0.02, 0.85, 0.91, 0.07)

    fontFamily: "Bahnschrift"
    letterSpacingWide: 2

    // Hard edges for hard light.
    radiusS: 0
    radiusM: 2
    radiusL: 3
    borderWidth: 2

    durFast:   120
    durNormal: 200
    durSlow:   300
    focusScale: 1.06

    backdropStyle: "scanlines"
    backdropTop:    "#3A1C71"
    backdropBottom: "#01010F"
    washA: "#FF2A6D"
    washB: "#05D9E8"
    glowStrength: 1.0

    dangerQuietBorder:  Qt.rgba(1, 0.16, 0.43, 0.6)
    dangerQuietTop:     "#3B0E28"
    dangerQuietBottom:  "#26081A"
    successQuietBorder: Qt.rgba(0.02, 1, 0.63, 0.6)
    successQuietTop:    "#06322A"
    successQuietBottom: "#04211C"

    // Grid under the scanlines — the retro horizon motif.
    texture: "grid"
    textureOpacity: 0.05
    textureColor: "#05D9E8"
}
