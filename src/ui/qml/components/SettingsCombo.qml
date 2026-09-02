import QtQuick
import QtQuick.Controls.Basic as QC
import QtQuick.Effects
import GameHQ

// Reusable Settings dropdown bound to a config.json key.
//
// `options` is an array of { label, value }. The stored config value is
// matched on load and every selection is persisted through app.setConfig —
// including selections made through the custom popup's delegate, which does
// NOT route through the ComboBox activate() path (the dev.52 pitfall that
// originally broke the replay-length dropdown).
QC.ComboBox {
    id: combo

    property var options: []
    property string configKey: ""
    property var defaultValue: undefined
    signal valueCommitted(var value)

    model: options
    textRole: "label"
    implicitWidth: Theme.s48 * 3 + Theme.s16
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontBody

    function commit(index) {
        currentIndex = index
        if (configKey.length > 0)
            app.setConfig(configKey, options[index].value)
        valueCommitted(options[index].value)
    }

    function refresh() {
        var cur = configKey.length > 0 ? app.config(configKey, defaultValue) : defaultValue
        for (var i = 0; i < options.length; ++i)
            if (options[i].value === cur) { currentIndex = i; break }
    }

    // ── Pad-driven dropdown ──
    // ComboBox.highlightedIndex is read-only and only tracks the mouse/keys, so
    // a gamepad cannot move it. This mirrors it: while padHighlight is >= 0 the
    // popup highlights that row instead, and nothing is written to config until
    // Cross commits — so Circle can back out without changing the setting.
    property int padHighlight: -1
    function padBeginHighlight() { padHighlight = currentIndex }
    function padStep(direction) {
        if (options.length === 0)
            return
        padHighlight = (padHighlight + direction + options.length) % options.length
    }
    function padCommitHighlighted() {
        if (padHighlight >= 0 && padHighlight < options.length)
            commit(padHighlight)
        padHighlight = -1
    }
    onVisibleChanged: if (!visible) padHighlight = -1
    Component.onCompleted: refresh()
    onOptionsChanged: refresh()
    onDefaultValueChanged: refresh()
    onActivated: commit(currentIndex)

    Connections {
        target: app
        function onConfigChanged(key, value) {
            if (key === combo.configKey) combo.refresh()
        }
        function onConfigGroupReset(prefix) {
            if (prefix.length === 0 || combo.configKey.startsWith(prefix + ".")) combo.refresh()
        }
    }

    contentItem: Text {
        text: combo.displayText
        color: Theme.text
        font: combo.font
        verticalAlignment: Text.AlignVCenter
        leftPadding: Theme.s12
        rightPadding: Theme.s24
        elide: Text.ElideRight
    }
    background: Rectangle {
        implicitHeight: Theme.fontBody + Theme.s16
        radius: Theme.radiusM
        color: Theme.surfaceAlt
        border.width: combo.activeFocus ? Theme.borderWidth + 1 : Theme.borderWidth
        // activeFocus matters as much as pressed/open: a pad or Tab lands here
        // without ever pressing it, and an unlit control reads as unreachable.
        border.color: (combo.pressed || combo.popup.visible || combo.activeFocus)
                      ? Theme.accent : Theme.stroke
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }
    }
    indicator: Text {
        x: combo.width - width - Theme.s12
        y: (combo.height - height) / 2
        text: "▾"
        color: Theme.textMuted
        font.pixelSize: Theme.fontBody
    }
    popup: QC.Popup {
        y: combo.height + Theme.s4
        width: combo.width
        implicitHeight: Math.min(contentItem.implicitHeight, Theme.s48 * 6)
        padding: 1
        // `clip: true` on the ListView only clips to the plain bounding box — it
        // does not respect the background's `radius`, so a highlighted row at
        // either end painted its square corners over the rounded ones. Mask the
        // list's rendered output to the same rounded shape (the CaptureTile
        // idiom), inset by the background's border so that border stays visible.
        contentItem: Item {
            implicitHeight: list.contentHeight

            Item {
                id: listLayer
                anchors.fill: parent
                layer.enabled: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: listMask
                }

                ListView {
                    id: list
                    anchors.fill: parent
                    clip: true
                    model: combo.popup.visible ? combo.delegateModel : null
                    // Follow the pad's highlight when it is driving, so the list
                    // scrolls to the row the user is on.
                    currentIndex: combo.padHighlight >= 0 ? combo.padHighlight
                                                          : combo.highlightedIndex
                }
            }

            Rectangle {
                id: listMask
                anchors.fill: listLayer
                radius: Math.max(0, Theme.radiusM - 1)   // popup radius less its border
                visible: false
                layer.enabled: true   // visible:false alone skips the scene graph,
                                      // leaving the mask empty (masks everything away)
            }
        }
        background: Rectangle {
            radius: Theme.radiusM
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke
        }
    }
    delegate: QC.ItemDelegate {
        width: combo.width
        highlighted: combo.padHighlight >= 0 ? combo.padHighlight === index
                                             : combo.highlightedIndex === index
        contentItem: Text {
            text: modelData.label
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontBody
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: highlighted ? Theme.surfaceAlt : "transparent"
        }
        // The custom popup's ListView doesn't route clicks through the
        // ComboBox's activate() path, so persist the choice here.
        onClicked: {
            combo.commit(index)
            combo.popup.close()
        }
    }
}
