// Light surfaces with the same brand hues, darkened enough to hold contrast
// against white instead of glowing on it.
//
// Not overridden (inherited from Skin.qml, deliberately): the scrims, the video
// chrome (badge/tile buttons/pulse) and focusGlow. Those sit over captures, not
// over app surfaces, so they stay dark here too.
Skin {
    //% "Light"
    label: qsTrId("gamehq.settings.theme.light.label")

    // Surfaces — bg0 is the page, surface is the raised card.
    bg0:        "#F4F6FB"
    bg1:        "#EAEEF7"
    surface:    "#FFFFFF"
    surfaceAlt: "#F0F3FA"
    stroke:     Qt.rgba(0, 0, 0, 0.09)
    borderLight: Qt.rgba(0, 0, 0, 0.24)
    // The overlay still floats over a game, so its panel keeps a light tint at
    // the same opacity the dark skin uses.
    panelTint:  Qt.rgba(0.957, 0.965, 0.980, 0.85)

    // Same hues as Dark, stepped darker: the dark skin's tints are tuned to glow
    // against navy and would read as pastel on white.
    accent1: "#7A56F5"
    accent2: "#1F8FE8"
    accent:  "#4F6FE8"

    text:      "#12172A"
    textMuted: "#5A6378"
    textFaint: "#98A0B3"

    // Darkened until they pass against white; the dark skin's danger/success
    // are tuned for a navy backdrop and vibrate on a light one.
    danger:  "#D42B44"
    success: "#1F9D52"
    warning: "#B87400"

    // The wash flips: a white overlay is invisible on white surfaces.
    highlight: "#000000"
    hoverTint: Qt.rgba(0, 0, 0, 0.04)

    // Quiet button variants become pale tints of the same hues.
    dangerQuietBorder:  Qt.rgba(0.83, 0.17, 0.27, 0.40)
    dangerQuietTop:     "#FDECEF"
    dangerQuietBottom:  "#F7DDE2"
    successQuietBorder: Qt.rgba(0.12, 0.62, 0.32, 0.40)
    successQuietTop:    "#E9F7EE"
    successQuietBottom: "#DCEFE4"
}
