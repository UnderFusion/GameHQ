import QtQuick
import GameHQ

// Overlay options (gear in the overlay sidebar): a floating card beside the
// sidebar, with no scrim, so the capture preview stays visible and every
// change shows live. The host owns the values and applies each change at once;
// this card only lists the rows, the pad/keyboard cursor and the mouse
// controls. Rows: control hints (toggle), four outer margins, interface size,
// thumbnail size and a layout reset.
Rectangle {
    id: root

    property bool open: false
    property int currentIndex: 0
    // { key: value } for the displayed values, supplied by the host.
    property var values: ({})

    signal closeRequested()
    signal rowHovered(int index)
    signal adjustRequested(string key, int direction)
    signal activateRequested(string key)

    readonly property var rows: [
        //% "Control hints"
        { key: "hints", kind: "toggle", label: qsTrId("gamehq.overlay.layout.hints") },
        //% "Left margin"
        { key: "margin_left", kind: "px", label: qsTrId("gamehq.overlay.layout.margin_left") },
        //% "Top margin"
        { key: "margin_top", kind: "px", label: qsTrId("gamehq.overlay.layout.margin_top") },
        //% "Right margin"
        { key: "margin_right", kind: "px", label: qsTrId("gamehq.overlay.layout.margin_right") },
        //% "Bottom margin"
        { key: "margin_bottom", kind: "px", label: qsTrId("gamehq.overlay.layout.margin_bottom") },
        //% "Interface size"
        { key: "scale", kind: "percent", label: qsTrId("gamehq.overlay.layout.interface_size") },
        //% "Thumbnail size"
        { key: "thumbs", kind: "percent", label: qsTrId("gamehq.overlay.layout.thumbnail_size") },
        //% "Reset overlay layout"
        { key: "reset", kind: "action", label: qsTrId("gamehq.overlay.layout.reset") }
    ]

    function currentKey() {
        return root.currentIndex >= 0 && root.currentIndex < root.rows.length
            ? root.rows[root.currentIndex].key : ""
    }

    //% "On"
    readonly property string onLabel: qsTrId("gamehq.overlay.layout.on")
    //% "Off"
    readonly property string offLabel: qsTrId("gamehq.overlay.layout.off")
    //% "%1 px"
    readonly property string pxFormat: qsTrId("gamehq.overlay.layout.px")

    function valueText(row) {
        const v = root.values[row.key]
        if (row.kind === "toggle")
            return v ? root.onLabel : root.offLabel
        if (row.kind === "px")
            return root.pxFormat.arg(v)
        if (row.kind === "percent")
            return v + "%"
        return ""
    }

    visible: root.open
    width: Theme.overlayLayoutPanelWidth
    height: Math.min(parent ? parent.height : implicitHeight, panelColumn.implicitHeight + Theme.s16 * 2)
    radius: Theme.radiusL
    color: Theme.surface
    border.width: 1
    border.color: Theme.stroke
    clip: true

    MouseArea { anchors.fill: parent }

    Flickable {
        anchors.fill: parent
        anchors.margins: Theme.s16
        contentHeight: panelColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height

        Column {
            id: panelColumn
            width: parent.width
            spacing: Theme.s4

            Item {
                width: parent.width
                height: Theme.s32

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.s8
                    anchors.right: closeButton.left
                    anchors.verticalCenter: parent.verticalCenter
                    //% "Overlay options"
                    text: qsTrId("gamehq.overlay.layout.title")
                    elide: Text.ElideRight
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontH3
                    font.weight: Font.DemiBold
                }

                Rectangle {
                    id: closeButton
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.s32
                    height: Theme.s32
                    radius: Theme.radiusS
                    color: closeMouse.containsMouse ? Theme.hoverTint : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        color: closeMouse.containsMouse ? Theme.text : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                    }
                    MouseArea {
                        id: closeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.closeRequested()
                    }
                }
            }

            Repeater {
                model: root.rows
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool current: root.currentIndex === row.index
                    width: panelColumn.width
                    height: Theme.s32 + Theme.s8
                    radius: Theme.radiusS
                    color: row.current ? Theme.surfaceAlt : "transparent"

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: root.rowHovered(row.index)
                        onClicked: {
                            root.rowHovered(row.index)
                            if (row.modelData.kind === "toggle" || row.modelData.kind === "action")
                                root.activateRequested(row.modelData.key)
                        }
                    }

                    Rectangle { // pad/keyboard cursor bar, as in the action menu
                        visible: row.current
                        width: 3; height: 18; radius: 2
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.accent
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.s12
                        anchors.right: controls.left
                        anchors.rightMargin: Theme.s8
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.label
                        elide: Text.ElideRight
                        color: row.current ? Theme.text : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                    }

                    Row {
                        id: controls
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.s8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.s4
                        visible: row.modelData.kind !== "action"

                        Repeater {
                            // Minus, value, plus for numbers; one switch-like
                            // value chip for the toggle.
                            model: row.modelData.kind === "toggle" ? ["value"] : ["minus", "value", "plus"]
                            delegate: Rectangle {
                                id: chip
                                required property string modelData
                                readonly property bool isValue: chip.modelData === "value"
                                width: chip.isValue ? Math.max(Theme.s48 + Theme.s8, chipText.implicitWidth + Theme.s16)
                                                    : Theme.s24 + Theme.s4
                                height: Theme.s24 + Theme.s4
                                radius: Theme.radiusS
                                color: chip.isValue ? "transparent"
                                     : chipMouse.containsMouse ? Theme.hoverTint : Theme.panelTint
                                border.width: chip.isValue ? 0 : 1
                                border.color: Theme.stroke

                                Text {
                                    id: chipText
                                    anchors.centerIn: parent
                                    text: chip.modelData === "minus" ? "−"
                                        : chip.modelData === "plus" ? "+"
                                        : root.valueText(row.modelData)
                                    color: chip.isValue && row.modelData.kind === "toggle"
                                           && root.values[row.modelData.key] ? Theme.accent : Theme.text
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontBody
                                    font.weight: chip.isValue ? Font.DemiBold : Font.Normal
                                }

                                MouseArea {
                                    id: chipMouse
                                    anchors.fill: parent
                                    enabled: !chip.isValue
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        root.rowHovered(row.index)
                                        root.adjustRequested(row.modelData.key,
                                                             chip.modelData === "plus" ? 1 : -1)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
