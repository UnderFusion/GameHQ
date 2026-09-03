// The Nord palette: desaturated arctic blue-greys with frost accents. Calm and
// low-contrast by design — the restful counterweight to Synthwave.
Skin {
    //% "Nord"
    label: qsTrId("gamehq.settings.theme.nord.label")
    //% "Desaturated arctic blue-grays. Calm and low-contrast."
    blurb: qsTrId("gamehq.settings.theme.nord.description")

    bg0:        "#2E3440"
    bg1:        "#3B4252"
    surface:    "#3B4252"
    surfaceAlt: "#434C5E"
    stroke:     Qt.rgba(1, 1, 1, 0.08)
    borderLight: Qt.rgba(0.85, 0.87, 0.91, 0.26)
    panelTint:  Qt.rgba(0.18, 0.204, 0.251, 0.90)

    accent1: "#88C0D0"
    accent2: "#5E81AC"
    accent:  "#81A1C1"

    text:      "#ECEFF4"
    textMuted: "#D8DEE9"
    textFaint: "#7B8494"
    textOnAccent: "#2E3440"

    danger:  "#BF616A"
    success: "#A3BE8C"
    warning: "#EBCB8B"

    focusGlow: Qt.rgba(0.53, 0.75, 0.82, 0.34)

    radiusS: 6
    radiusM: 10
    radiusL: 14

    durFast:   150
    durNormal: 230
    durSlow:   330
    focusScale: 1.03

    backdropStyle: "gradient"
    backdropTop:    "#3B4252"
    backdropBottom: "#272C36"
    glowStrength: 0.2

    dangerQuietBorder:  Qt.rgba(0.75, 0.38, 0.42, 0.45)
    dangerQuietTop:     "#40323A"
    dangerQuietBottom:  "#33282F"
    successQuietBorder: Qt.rgba(0.64, 0.75, 0.55, 0.45)
    successQuietTop:    "#3A4238"
    successQuietBottom: "#2F352E"

    // A whisper of grain so the flat arctic greys are not perfectly dead.
    texture: "grain"
    textureOpacity: 0.03
    textureColor: "#ECEFF4"
}
