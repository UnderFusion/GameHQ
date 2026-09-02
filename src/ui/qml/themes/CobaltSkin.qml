// Chat-app ergonomics: soft neutral greys, a saturated indigo, very generous
// rounding and fast micro-interactions. The snappiest skin in the set.
Skin {
    //% "Cobalt"
    label: qsTrId("gamehq.settings.theme.cobalt.label")
    //% "Soft greys, indigo accent, pill-round and very fast."
    blurb: qsTrId("gamehq.settings.theme.cobalt.description")

    bg0:        "#1E1F22"
    bg1:        "#232428"
    surface:    "#2B2D31"
    surfaceAlt: "#313338"
    stroke:     Qt.rgba(1, 1, 1, 0.08)
    borderLight: Qt.rgba(1, 1, 1, 0.20)
    panelTint:  Qt.rgba(0.137, 0.141, 0.157, 0.92)

    accent1: "#7A84F7"
    accent2: "#5865F2"
    accent:  "#5865F2"

    text:      "#DBDEE1"
    textMuted: "#B5BAC1"
    textFaint: "#80848E"

    danger:  "#DA373C"
    success: "#23A55A"
    warning: "#F0B232"

    focusGlow: Qt.rgba(0.35, 0.40, 0.95, 0.40)
    hoverTint: Qt.rgba(1, 1, 1, 0.04)

    fontFamily: "Segoe UI"

    // Pill-soft.
    radiusS: 8
    radiusM: 16
    radiusL: 24

    // Micro-interactions, not transitions.
    durFast:   90
    durNormal: 140
    durSlow:   200
    focusScale: 1.04

    backdropStyle: "flat"
    backdropTop:    "#1E1F22"
    backdropBottom: "#1E1F22"
    glowStrength: 0.25
}
