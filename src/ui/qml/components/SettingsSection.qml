import QtQuick
import QtQuick.Layouts
import GameHQ

Rectangle {
    id: root

    property string eyebrow: ""
    property string title: ""
    property string description: ""
    property string icon: ""
    property string badge: ""
    property string status: ""
    property string variant: "normal" // normal | status | warning | compact
    property bool showHeaderDivider: contentColumn.children.length > 0
    property int contentSpacing: Theme.s4
    property int titlePixelSize: compact ? Theme.fontBody : Theme.fontH3
    property Component headerAction
    default property alias contentData: contentColumn.data

    readonly property bool compact: variant === "compact"
    readonly property color toneColor: variant === "warning" ? Theme.warning
                                          : variant === "status" ? Theme.success
                                                                 : Theme.accent
    property color statusColor: toneColor
    readonly property var focusedItem: Window.window ? Window.window.activeFocusItem : null
    readonly property bool controllerFocused: Window.window
                                              && Window.window.usingGamepad
                                              && containsItem(focusedItem)

    function containsItem(item) {
        for (let probe = item; probe; probe = probe.parent)
            if (probe === root)
                return true
        return false
    }

    Layout.fillWidth: true
    Layout.minimumWidth: 0
    implicitHeight: cardLayout.implicitHeight + (compact ? Theme.s12 : Theme.s16) * 2
    radius: Theme.radiusM
    color: controllerFocused && variant !== "warning"
           ? Theme.surfaceElevated
           : variant === "warning" ? Theme.warningSoft : Theme.surface
    border.width: controllerFocused ? Theme.borderWidth + 1 : Theme.borderWidth
    border.color: controllerFocused
                  ? Theme.focusRing
                  : variant === "warning"
                    ? Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.45)
                    : Theme.stroke

    Behavior on color {
        ColorAnimation { duration: Theme.durFast }
    }

    ColumnLayout {
        id: cardLayout
        anchors.fill: parent
        anchors.margins: root.compact ? Theme.s12 : Theme.s16
        spacing: Theme.s8

        RowLayout {
            id: headerRow
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.s12
            visible: root.eyebrow.length > 0 || root.title.length > 0
                     || root.description.length > 0 || root.icon.length > 0
                     || root.badge.length > 0 || root.status.length > 0
                     || root.headerAction !== null

            Rectangle {
                visible: root.icon.length > 0
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: Theme.radiusS
                color: Theme.accentSoft
                Text {
                    anchors.centerIn: parent
                    text: root.icon
                    color: root.toneColor
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontH3
                    font.weight: Font.DemiBold
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: Theme.s4
                Text {
                    visible: root.eyebrow.length > 0
                    text: root.eyebrow.toUpperCase()
                    color: root.toneColor
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    font.letterSpacing: Theme.letterSpacingWide
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: Theme.s8
                    Text {
                        visible: root.title.length > 0
                        text: root.title
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: root.titlePixelSize
                        font.weight: Font.DemiBold
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                    }
                    Rectangle {
                        visible: root.badge.length > 0
                        implicitWidth: badgeText.implicitWidth + Theme.s12
                        implicitHeight: badgeText.implicitHeight + Theme.s4
                        radius: Theme.radiusPill
                        color: Theme.surfaceElevated
                        Text {
                            id: badgeText
                            anchors.centerIn: parent
                            text: root.badge
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                    Text {
                        visible: root.status.length > 0
                        text: root.status
                        color: root.statusColor
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                }
                Text {
                    visible: root.description.length > 0
                    text: root.description
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
            }

            Loader {
                visible: status === Loader.Ready
                sourceComponent: root.headerAction
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
            }
        }

        Rectangle {
            visible: root.showHeaderDivider && headerRow.visible
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.divider
        }

        ColumnLayout {
            id: contentColumn
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: root.contentSpacing
        }
    }
}
