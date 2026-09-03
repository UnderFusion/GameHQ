import QtQuick
import QtQuick.Layouts
import GameHQ

// Compact desktop-only notice. AboutWhatsNewDialog owns release summaries,
// full notes, and update choices so the app never maintains two changelog UIs.
Rectangle {
    id: root

    property bool dismissed: false
    signal detailsRequested()

    onVisibleChanged: if (visible) root.dismissed = false

    visible: updates.latestVersion !== ""
             && ["UpdateAvailable", "Downloading", "ReadyToInstall", "PreparingForUpdate",
                 "Quiescent", "Installing", "Failed"].includes(updates.stateName)
             && !root.dismissed
    height: visible ? implicitHeight : 0
    implicitHeight: content.implicitHeight + Theme.s12 * 2
    color: Theme.surface
    radius: Theme.radiusM
    border.width: 1
    border.color: Theme.accent
    clip: true

    Behavior on height {
        NumberAnimation { duration: Theme.durNormal; easing.type: Easing.OutCubic }
    }

    function formattedSize(bytes) {
        languageManager.translationRevision
        if (bytes >= 1024 * 1024) {
            //% "%1 MB"
            return qsTrId("gamehq.format.size.megabytes")
                    .arg(languageManager.formatDecimal(bytes / (1024 * 1024), 1))
        }
        if (bytes >= 1024) {
            //% "%1 KB"
            return qsTrId("gamehq.format.size.kilobytes")
                    .arg(languageManager.formatInteger(Math.round(bytes / 1024)))
        }
        //% "%1 B"
        return qsTrId("gamehq.format.size.bytes").arg(languageManager.formatInteger(bytes))
    }

    function titleText() {
        switch (updates.stateName) {
        case "Downloading":
            //% "Downloading %1 %2"
            return qsTrId("gamehq.update.banner.downloading").arg(Brand.name).arg(updates.latestVersion)
        case "ReadyToInstall":
            //% "%1 %2 is ready"
            return qsTrId("gamehq.update.banner.ready").arg(Brand.name).arg(updates.latestVersion)
        case "PreparingForUpdate":
        case "Quiescent":
            //% "Preparing %1 %2"
            return qsTrId("gamehq.update.banner.preparing").arg(Brand.name).arg(updates.latestVersion)
        case "Installing":
            //% "Installing %1 %2"
            return qsTrId("gamehq.update.banner.installing").arg(Brand.name).arg(updates.latestVersion)
        case "Failed":
            //% "The update needs attention"
            return qsTrId("gamehq.update.banner.needs_attention")
        default:
            //% "%1 %2 is available"
            return qsTrId("gamehq.update.banner.available").arg(Brand.name).arg(updates.latestVersion)
        }
    }

    function detailText() {
        switch (updates.stateName) {
        case "Downloading":
            //% "%1% complete"
            return qsTrId("gamehq.update.progress_percent")
                    .arg(languageManager.formatInteger(updates.progress))
        case "ReadyToInstall":
            //% "Download verified and ready to install."
            return qsTrId("gamehq.update.banner.download_verified")
        case "PreparingForUpdate":
        case "Quiescent":
            //% "Waiting for capture work to finish safely."
            return qsTrId("gamehq.update.banner.waiting_for_capture")
        case "Installing":
            //% "GameHQ will restart when installation finishes."
            return qsTrId("gamehq.update.banner.restart_when_finished")
        case "Failed": return updates.errorText !== "" ? updates.errorText
                                                         //% "Open the update details to continue."
                                                         : qsTrId("gamehq.update.banner.open_details")
        default:
            const date = languageManager.formatDate(updates.publishedAt)
            const size = updates.size > 0 ? root.formattedSize(updates.size) : ""
            if (date !== "" && size !== "") {
                //% "%1 · %2"
                return qsTrId("gamehq.update.release_metadata").arg(date).arg(size)
            }
            return date !== "" ? date : size
        }
    }

    RowLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.margins: Theme.s12
        spacing: Theme.s12

        Rectangle {
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40
            radius: Theme.radiusM
            color: Theme.hoverTint

            Image {
                anchors.centerIn: parent
                width: 26
                height: 26
                source: "qrc:/icons/gamehq.svg"
                sourceSize.width: 26
                sourceSize.height: 26
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.s4

            Text {
                Layout.fillWidth: true
                text: root.titleText()
                textFormat: Text.PlainText
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontH3
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: root.detailText()
                textFormat: Text.PlainText
                color: updates.stateName === "Failed" ? Theme.danger : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
        }

        AccentButton {
            visible: ["UpdateAvailable", "Downloading", "ReadyToInstall", "Failed"]
                     .includes(updates.stateName)
            label: {
                if (updates.stateName === "UpdateAvailable") {
                    //% "See what's new"
                    return qsTrId("gamehq.update.see_whats_new")
                }
                //% "View update"
                return qsTrId("gamehq.update.view_update")
            }
            primary: updates.stateName === "UpdateAvailable"
            onClicked: root.detailsRequested()
        }
        AccentButton {
            visible: updates.stateName === "Downloading" || updates.stateName === "ReadyToInstall"
            label: {
                if (updates.stateName === "Downloading") {
                    //% "Cancel"
                    return qsTrId("gamehq.action.cancel")
                }
                //% "Install and restart"
                return qsTrId("gamehq.update.install_and_restart")
            }
            primary: updates.stateName === "ReadyToInstall"
            onClicked: updates.stateName === "Downloading"
                       ? updates.cancelDownload() : updates.installAndRestart()
        }
        AccentButton {
            visible: updates.stateName === "UpdateAvailable" || updates.stateName === "Failed"
                     || updates.stateName === "ReadyToInstall"
            //% "Not now"
            label: qsTrId("gamehq.action.not_now")
            onClicked: root.dismissed = true
        }
    }
}
