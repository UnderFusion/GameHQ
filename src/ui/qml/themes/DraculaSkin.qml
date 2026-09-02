// The Dracula palette: purple-grey base with candy-saturated accents. High
// contrast and playful where Nord is muted.
Skin {
    //% "Dracula"
    label: qsTrId("gamehq.settings.theme.dracula.label")
    //% "Purple-grey base with bright candy accents."
    blurb: qsTrId("gamehq.settings.theme.dracula.description")

    bg0:        "#21222C"
    bg1:        "#282A36"
    surface:    "#282A36"
    surfaceAlt: "#343746"
    stroke:     Qt.rgba(1, 1, 1, 0.08)
    borderLight: Qt.rgba(0.74, 0.58, 0.98, 0.32)
    panelTint:  Qt.rgba(0.157, 0.165, 0.212, 0.90)

    accent1: "#FF79C6"
    accent2: "#8BE9FD"
    accent:  "#BD93F9"

    text:      "#F8F8F2"
    textMuted: "#BFC3D4"
    textFaint: "#6272A4"
    textOnAccent: "#21222C"

    danger:  "#FF5555"
    success: "#50FA7B"
    warning: "#FFB86C"

    focusGlow: Qt.rgba(0.74, 0.58, 0.98, 0.45)
    hoverTint: Qt.rgba(1, 1, 1, 0.05)

    radiusS: 6
    radiusM: 10
    radiusL: 14

    durFast:   120
    durNormal: 190
    durSlow:   270
    focusScale: 1.05

    backdropStyle: "gradient"
    backdropTop:    "#2C2E3E"
    backdropBottom: "#1A1B23"
    glowStrength: 0.45

    dangerQuietBorder:  Qt.rgba(1, 0.33, 0.33, 0.45)
    dangerQuietTop:     "#3B2833"
    dangerQuietBottom:  "#2C1F28"
    successQuietBorder: Qt.rgba(0.31, 0.98, 0.48, 0.45)
    successQuietTop:    "#25382F"
    successQuietBottom: "#1D2B25"

    texture: "grain"
    textureOpacity: 0.03
    textureColor: "#F8F8F2"
}
