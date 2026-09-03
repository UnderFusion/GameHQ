import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as QC
import GameHQ

// One clearly labeled assignment slot. Edit/Add is the dominant interaction;
// Remove stays inside the same card so it cannot be mistaken for another slot.
Rectangle {
    id: root

    function directionalActionLabel(value) {
        const upper = value.toUpperCase()
        return languageManager.layoutDirection === Qt.RightToLeft
                ? upper.replace(/›/g, "‹").replace(/→/g, "←") : upper
    }

    property string slotLabel: ""
    property string triggerLabel: ""
    property string badgeLabel: ""
    property bool assigned: false
    property bool editable: true
    property string changeState: "default"
    property string statusLabel: ""
    readonly property bool changed: changeState !== "default"
    signal editRequested()
    signal clearRequested()
    signal resetRequested()

    implicitHeight: Theme.s48 + Theme.s24
    radius: Theme.radiusM
    color: root.changed ? Theme.accentSoft : root.assigned ? Theme.surface : "transparent"
    border.width: Theme.borderWidth
    border.color: root.changed ? Theme.accent : root.assigned ? Theme.borderLight : Theme.stroke

    // Half-height and vertically centred so the accent bar never pokes past
    // the card's rounded left corners.
    Rectangle {
        visible: root.changed
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 3
        height: parent.height / 2
        radius: width / 2
        color: Theme.accent
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.s8
        spacing: Theme.s4

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.s24
            spacing: Theme.s8

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s8
                //% "%1 assignment"
                text: qsTrId("gamehq.input.binding_card.heading")
                    .arg(root.slotLabel).toUpperCase()
                color: Theme.textFaint
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                font.letterSpacing: Theme.letterSpacingWide
                elide: Text.ElideRight
            }

            Rectangle {
                visible: root.statusLabel !== ""
                implicitWidth: statusText.implicitWidth + Theme.s12
                implicitHeight: statusText.implicitHeight + Theme.s4
                radius: Theme.radiusPill
                color: Theme.accentSoft
                border.width: Theme.borderWidth
                border.color: Theme.accent

                Text {
                    id: statusText
                    anchors.centerIn: parent
                    text: root.statusLabel
                    color: Theme.accent
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                }
            }

            QC.AbstractButton {
                id: resetButton

                visible: root.editable
                         && (root.changeState === "modified" || root.changeState === "removed")
                Layout.preferredWidth: resetText.implicitWidth + Theme.s16
                Layout.preferredHeight: Theme.s24
                focusPolicy: Qt.StrongFocus
                Accessible.name: {
                    if (root.changeState === "removed") {
                        //% "Restore %1 assignment"
                        return qsTrId("gamehq.input.binding_card.restore_accessible").arg(root.slotLabel)
                    }
                    //% "Revert %1 assignment"
                    return qsTrId("gamehq.input.binding_card.revert_accessible").arg(root.slotLabel)
                }
                Accessible.role: Accessible.Button
                onClicked: root.resetRequested()

                contentItem: Text {
                    id: resetText
                    text: {
                        if (root.changeState === "removed") {
                            //% "Restore"
                            return qsTrId("gamehq.action.restore")
                        }
                        //% "Revert"
                        return qsTrId("gamehq.action.revert")
                    }
                    color: resetButton.activeFocus || resetButton.hovered
                           ? Theme.accent : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: Theme.radiusS
                    color: resetButton.hovered || resetButton.down ? Theme.accentSoft : "transparent"
                    border.width: resetButton.activeFocus ? Theme.borderWidth + 1 : 0
                    border.color: Theme.focusRing
                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                }
            }

            QC.AbstractButton {
                id: clearButton

                visible: root.assigned && root.editable
                Layout.preferredWidth: removeText.implicitWidth + Theme.s16
                Layout.preferredHeight: Theme.s24
                focusPolicy: Qt.StrongFocus
                //% "Remove %1 assignment"
                Accessible.name: qsTrId("gamehq.input.binding_card.remove_accessible").arg(root.slotLabel)
                Accessible.role: Accessible.Button
                onClicked: root.clearRequested()

                contentItem: Text {
                    id: removeText
                    //% "Remove"
                    text: qsTrId("gamehq.action.remove")
                    color: clearButton.activeFocus || clearButton.hovered
                           ? Theme.danger : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: Theme.radiusS
                    color: clearButton.hovered || clearButton.down ? Theme.dangerSoft : "transparent"
                    border.width: clearButton.activeFocus ? Theme.borderWidth + 1 : 0
                    border.color: Theme.focusRing
                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                }
            }
        }

        QC.AbstractButton {
            id: valueField

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            enabled: root.editable
            focusPolicy: root.editable ? Qt.StrongFocus : Qt.NoFocus
            leftPadding: Theme.s8
            rightPadding: Theme.s8
            Accessible.name: {
                if (root.assigned && root.statusLabel !== "") {
                    //% "Edit %1 assignment: %2, %3, status %4"
                    return qsTrId("gamehq.input.binding_card.edit_status_accessible")
                        .arg(root.slotLabel).arg(root.triggerLabel)
                        .arg(root.badgeLabel).arg(root.statusLabel)
                }
                if (root.assigned) {
                    //% "Edit %1 assignment: %2, %3"
                    return qsTrId("gamehq.input.binding_card.edit_accessible")
                        .arg(root.slotLabel).arg(root.triggerLabel).arg(root.badgeLabel)
                }
                if (root.statusLabel !== "") {
                    //% "Add %1 assignment, status %2"
                    return qsTrId("gamehq.input.binding_card.add_status_accessible")
                        .arg(root.slotLabel).arg(root.statusLabel)
                }
                //% "Add %1 assignment"
                return qsTrId("gamehq.input.binding_card.add_accessible").arg(root.slotLabel)
            }
            Accessible.role: Accessible.Button
            onClicked: root.editRequested()

            contentItem: RowLayout {
                spacing: Theme.s8

                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: {
                        if (root.assigned)
                            return root.triggerLabel
                        if (root.changeState === "removed") {
                            //% "Unassigned"
                            return qsTrId("gamehq.input.binding_card.unassigned")
                        }
                        //% "+ Add input"
                        return qsTrId("gamehq.input.binding_card.add_input")
                    }
                    color: root.assigned ? Theme.text : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    font.weight: root.assigned ? Font.DemiBold : Font.Normal
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }

                Rectangle {
                    visible: root.assigned && root.badgeLabel !== ""
                    implicitWidth: badgeText.implicitWidth + Theme.s12
                    implicitHeight: badgeText.implicitHeight + Theme.s4
                    radius: Theme.radiusPill
                    color: Theme.bg1
                    border.width: Theme.borderWidth
                    border.color: Theme.stroke

                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: root.badgeLabel
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                    }
                }

                Text {
                    text: {
                        if (!root.editable) {
                            //% "Fixed"
                            return qsTrId("gamehq.settings.input.bindings.fixed").toUpperCase()
                        }
                        if (root.assigned) {
                            //% "Edit  ›"
                            return root.directionalActionLabel(qsTrId("gamehq.input.binding_card.edit"))
                        }
                        //% "Add  ›"
                        return root.directionalActionLabel(qsTrId("gamehq.input.binding_card.add"))
                    }
                    color: valueField.activeFocus || valueField.hovered
                           ? Theme.accent : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                    font.letterSpacing: Theme.letterSpacingWide
                }
            }

            background: Rectangle {
                radius: Theme.radiusS
                color: valueField.down || valueField.hovered ? Theme.surfaceHover : "transparent"
                border.width: valueField.activeFocus ? Theme.borderWidth + 1 : 0
                border.color: Theme.focusRing
                Behavior on color { ColorAnimation { duration: Theme.durFast } }
            }
        }
    }
}
