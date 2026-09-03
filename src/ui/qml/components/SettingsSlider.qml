import QtQuick
import QtQuick.Controls.Basic as QC
import GameHQ

// Reusable Settings slider bound to a config.json key, storing an integer.
// The handle follows a drag live, but the value is persisted only when the
// handle is released — app.setConfig writes config.json on every call, so
// committing per drag-tick would hammer the disk. Discrete moves (keyboard
// arrows, clicking the track) commit immediately.
Row {
    id: root
    property string configKey: ""
    property int defaultValue: 0
    property alias from: slider.from
    property alias to: slider.to
    property alias stepSize: slider.stepSize
    // Renders the current value next to the track; override for other units.
    property var formatValue: function (v) {
        languageManager.translationRevision
        //% "%1%"
        return qsTrId("gamehq.format.percent")
                .arg(languageManager.formatInteger(Math.round(v)))
    }

    spacing: Theme.s12

    function refresh() {
        slider.value = configKey.length > 0 ? Number(app.config(configKey, defaultValue))
                                            : defaultValue
    }
    function commit() {
        if (configKey.length > 0)
            app.setConfig(configKey, Math.round(slider.value))
    }
    Component.onCompleted: refresh()

    Connections {
        target: app
        function onConfigChanged(key, value) {
            if (key === root.configKey && !slider.pressed) slider.value = Number(value)
        }
        function onConfigGroupReset(prefix) {
            if (prefix.length === 0 || root.configKey.startsWith(prefix + ".")) root.refresh()
        }
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: root.formatValue(slider.value)
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        // Widest expected value ("150%") so the track doesn't shift as the
        // label's width changes while dragging.
        horizontalAlignment: Text.AlignRight
        width: Theme.s48 - Theme.s8
    }

    QC.Slider {
        id: slider
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.s48 * 3 + Theme.s16   // match SettingsCombo
        focusPolicy: Qt.StrongFocus

        onMoved: if (!pressed) root.commit()
        onPressedChanged: if (!pressed) root.commit()

        background: Rectangle {
            x: slider.leftPadding
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            width: slider.availableWidth
            height: Theme.s4
            radius: Theme.s4 / 2
            color: Theme.surfaceAlt

            // Filled portion left of the handle.
            Rectangle {
                width: slider.visualPosition * parent.width
                height: parent.height
                radius: parent.radius
                color: Theme.accent
            }
        }

        handle: Rectangle {
            x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
            y: slider.topPadding + slider.availableHeight / 2 - height / 2
            implicitWidth: Theme.s16 - 2
            implicitHeight: Theme.s16 - 2
            radius: Theme.radiusPill
            color: slider.pressed ? Theme.accent1 : Theme.accent
            border.width: 1
            // Same focus affordance as SettingsToggle: a pad or Tab lands here
            // without pressing, and an unlit control reads as unreachable.
            border.color: slider.activeFocus ? Theme.accent2 : Theme.borderLight
        }
    }
}
