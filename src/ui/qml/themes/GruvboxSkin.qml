// The Gruvbox palette: warm retro-terminal browns and creams with low-saturation
// earth accents. Monospaced type completes the vintage-console feel — the only
// skin here that is warm rather than cool.
Skin {
    //% "Gruvbox"
    label: qsTrId("gamehq.settings.theme.gruvbox.label")
    //% "Warm retro-terminal browns and cream, in monospace."
    blurb: qsTrId("gamehq.settings.theme.gruvbox.description")

    bg0:        "#1D2021"
    bg1:        "#282828"
    surface:    "#32302F"
    surfaceAlt: "#3C3836"
    stroke:     Qt.rgba(1, 0.95, 0.85, 0.09)
    borderLight: Qt.rgba(0.92, 0.86, 0.70, 0.28)
    panelTint:  Qt.rgba(0.114, 0.125, 0.129, 0.90)

    accent1: "#FABD2F"
    accent2: "#D79921"
    accent:  "#D79921"

    text:      "#EBDBB2"
    textMuted: "#BDAE93"
    textFaint: "#928374"
    textOnAccent: "#1D2021"

    danger:  "#CC241D"
    success: "#8EC07C"
    warning: "#FE8019"

    focusGlow: Qt.rgba(0.84, 0.60, 0.13, 0.40)
    highlight: "#FBF1C7"
    hoverTint: Qt.rgba(1, 0.95, 0.85, 0.05)

    fontFamily: "Cascadia Mono"

    radiusS: 3
    radiusM: 5
    radiusL: 7

    durFast:   130
    durNormal: 200
    durSlow:   290
    focusScale: 1.03

    backdropStyle: "gradient"
    backdropTop:    "#32302F"
    backdropBottom: "#1D2021"
    glowStrength: 0.3

    dangerQuietBorder:  Qt.rgba(0.80, 0.14, 0.11, 0.45)
    dangerQuietTop:     "#3A2A26"
    dangerQuietBottom:  "#2C201E"
    successQuietBorder: Qt.rgba(0.56, 0.75, 0.49, 0.45)
    successQuietTop:    "#2F3A2C"
    successQuietBottom: "#242C22"

    // Paper grain: the warm retro-terminal look leans on it.
    texture: "grain"
    textureOpacity: 0.05
    textureColor: "#FBF1C7"
}
