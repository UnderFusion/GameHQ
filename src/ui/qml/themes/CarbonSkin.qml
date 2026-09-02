// Storefront-minimal: layered neutral greys, one bright blue, barely any
// borders. The quietest skin here — closest to "no theme at all".
Skin {
    //% "Carbon"
    label: qsTrId("gamehq.settings.theme.carbon.label")
    //% "Layered neutral greys and one bright blue. Minimal to a fault."
    blurb: qsTrId("gamehq.settings.theme.carbon.description")

    bg0:        "#121212"
    bg1:        "#1B1B1B"
    surface:    "#202020"
    surfaceAlt: "#2B2B2B"
    stroke:     Qt.rgba(1, 1, 1, 0.07)
    borderLight: Qt.rgba(1, 1, 1, 0.20)
    panelTint:  Qt.rgba(0.106, 0.106, 0.106, 0.90)

    accent1: "#25BCFF"
    accent2: "#0E7EEF"
    accent:  "#1FA6F5"

    text:      "#F1F1F1"
    textMuted: "#9C9C9C"
    textFaint: "#6A6A6A"
    textOnAccent: "#06121B"

    danger:  "#F04A5A"
    success: "#3FCF6B"
    warning: "#F5B740"

    focusGlow: Qt.rgba(0.12, 0.65, 0.96, 0.34)

    radiusS: 4
    radiusM: 6
    radiusL: 8

    durFast:   130
    durNormal: 200
    durSlow:   280
    focusScale: 1.03

    backdropStyle: "flat"
    backdropTop:    "#121212"
    backdropBottom: "#121212"
    glowStrength: 0.2

    // Carbon fiber, at the threshold of visible — the skin is named for it.
    texture: "weave"
    textureOpacity: 0.035
    textureColor: "#FFFFFF"
}
