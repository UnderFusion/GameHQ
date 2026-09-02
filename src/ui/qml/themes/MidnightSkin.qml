// Console-dashboard mood: near-black base with soft blue gradient washes
// floating behind the content, medium-rounded cards, and slow eased motion with
// a pronounced focus lift. The most "cinematic" skin — deliberately unhurried
// where Cobalt and Emerald are snappy.
Skin {
    //% "Midnight"
    label: qsTrId("gamehq.settings.theme.midnight.label")
    //% "Near-black with soft blue washes and slow, cinematic motion."
    blurb: qsTrId("gamehq.settings.theme.midnight.description")

    bg0:        "#04060F"
    bg1:        "#0A1024"
    surface:    "#101833"
    surfaceAlt: "#162042"
    stroke:     Qt.rgba(1, 1, 1, 0.10)
    borderLight: Qt.rgba(1, 1, 1, 0.26)
    panelTint:  Qt.rgba(0.024, 0.043, 0.102, 0.86)

    accent1: "#5CC9FB"
    accent2: "#1E5DDB"
    accent:  "#2E8BF5"

    text:      "#F2F5FA"
    textMuted: "#93A3BE"
    textFaint: "#5D6C88"

    danger:  "#FF3B3B"
    success: "#00B16A"
    warning: "#FFC24D"

    focusGlow: Qt.rgba(0.18, 0.55, 0.96, 0.55)

    radiusS: 10
    radiusM: 14
    radiusL: 20

    // Slow and heavily eased, with a bigger lift on focus — the opposite of a
    // chat app's instant snap.
    durFast:   200
    durNormal: 340
    durSlow:   520
    focusScale: 1.07

    backdropStyle: "wash"
    backdropTop:    "#0A1330"
    backdropBottom: "#02030A"
    washA: "#1E5DDB"
    washB: "#5CC9FB"
    glowStrength: 0.9

    // Grain only — the blue washes already carry this skin.
    texture: "grain"
    textureOpacity: 0.025
    textureColor: "#5CC9FB"
}
