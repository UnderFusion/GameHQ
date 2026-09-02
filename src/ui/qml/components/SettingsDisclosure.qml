import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as QC
import GameHQ

ColumnLayout {
    id: root
    //% "View technical details"
    property string label: qsTrId("gamehq.settings.disclosure.default")
    property bool expanded: false
    default property alias contentData: details.data
    Layout.fillWidth: true
    spacing: Theme.s8

    QC.AbstractButton {
        id: disclosureButton
        Layout.fillWidth: true
        implicitHeight: 44
        leftPadding: Theme.s16
        rightPadding: Theme.s8
        topPadding: Theme.s8
        bottomPadding: Theme.s8
        focusPolicy: Qt.StrongFocus
        onClicked: root.expanded = !root.expanded

        background: Rectangle {
            radius: Theme.radiusS
            color: disclosureButton.down || root.expanded
                   ? Theme.accentSoft
                   : disclosureButton.hovered ? Theme.surfaceHover
                                              : Theme.surfaceElevated
            border.width: disclosureButton.activeFocus ? Theme.borderWidth + 1
                                                       : Theme.borderWidth
            border.color: disclosureButton.activeFocus || root.expanded
                          ? Theme.accent : Theme.stroke
        }

        contentItem: RowLayout {
            spacing: Theme.s12

            Text {
                text: root.label
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Text {
                text: {
                    if (root.expanded) {
                        //% "Hide"
                        return qsTrId("gamehq.settings.disclosure.hide")
                    }
                    //% "Show"
                    return qsTrId("gamehq.settings.disclosure.show")
                }
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
            }

            Rectangle {
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                radius: Theme.radiusS
                color: Theme.accentSoft

                Text {
                    anchors.centerIn: parent
                    text: root.expanded ? "\u25B4" : "\u25BE"
                    color: Theme.accent
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    ColumnLayout {
        id: details
        visible: root.expanded
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.s8
    }
}
