import QtQuick

// Each window owns its logical viewport. Native geometry and Qt's monitor DPI
// remain untouched; every child, including hit targets, uses the same transform.
Item {
    id: root
    required property var settings
    required property string configKey
    required property real minimumContentWidth
    required property real minimumContentHeight
    property int requestedPercent: 100
    // Accepted interface-size presets; anything else falls back to 100%. The
    // overlay adds its smaller 75% and 90% steps (overlayPresets).
    readonly property var standardPresets: [100, 125, 150, 175, 200]
    readonly property var overlayPresets: [75, 90, 100, 125, 150, 175, 200]
    property var presets: standardPresets
    readonly property real effectiveScale: Math.max(0.1, Math.min(
        requestedPercent / 100, width / minimumContentWidth, height / minimumContentHeight))
    default property alias contentData: viewport.data
    readonly property alias contentItem: viewport
    readonly property Scale scaleTransform: Scale {
        xScale: root.effectiveScale
        yScale: root.effectiveScale
    }

    function refresh() {
        const value = Number(settings.config(configKey, 100))
        requestedPercent = presets.indexOf(value) >= 0 ? value : 100
    }
    Component.onCompleted: refresh()
    onConfigKeyChanged: if (settings) refresh()
    onPresetsChanged: if (settings) refresh()

    readonly property Connections configWatch: Connections {
        target: root.settings
        function onConfigChanged(key, value) {
            if (key === root.configKey) root.refresh()
        }
        function onConfigGroupReset(prefix) {
            if (!prefix.length || root.configKey.startsWith(prefix + ".")) root.refresh()
        }
    }

    readonly property Item logicalContent: Item {
        id: viewport
        parent: root
        width: root.width / root.effectiveScale
        height: root.height / root.effectiveScale
        transform: root.scaleTransform
    }
}
