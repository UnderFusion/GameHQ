import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import GameHQ
import "../components"

SettingsPage {
    id: root
    //% "Capture"
    pageTitle: qsTrId("gamehq.settings.capture.title")
    //% "Choose when, how, and where GameHQ saves screenshots."
    pageDescription: qsTrId("gamehq.settings.capture.description")
    property string locationError: ""

    function finishLocationChange(error) {
        locationError = error
        sounds.play(error.length > 0 ? "error" : "confirm")
    }

    function openFolder(path) {
        Qt.openUrlExternally("file:///" + path.replace(/\\/g, "/"))
    }

    SettingsSection {
        //% "Capture mode"
        eyebrow: qsTrId("gamehq.settings.capture.mode.eyebrow")
        //% "When to capture"
        title: qsTrId("gamehq.settings.capture.mode.title")
        //% "Control when screenshots and replay recording are allowed."
        description: qsTrId("gamehq.settings.capture.mode.description")
        SettingsRow {
            //% "Capture mode"
            label: qsTrId("gamehq.settings.capture.mode.label")
            //% "Only in games is the safest default for global shortcuts."
            description: qsTrId("gamehq.settings.capture.mode.row_description")
            SettingsCombo {
                configKey: "capture.mode"
                defaultValue: "only_in_games"
                options: [
                    //% "Only in games"
                    { label: qsTrId("gamehq.settings.capture.mode.only_in_games"), value: "only_in_games" },
                    //% "Whitelisted games"
                    { label: qsTrId("gamehq.settings.capture.mode.whitelist"), value: "whitelist" },
                    //% "Always"
                    { label: qsTrId("gamehq.settings.capture.mode.always"), value: "always" }
                ]
            }
        }
    }

    SettingsSection {
        //% "Image"
        eyebrow: qsTrId("gamehq.settings.capture.image.eyebrow")
        //% "Format and quality"
        title: qsTrId("gamehq.settings.capture.image.title")
        //% "PNG is lossless; JPEG trades some quality for smaller files."
        description: qsTrId("gamehq.settings.capture.image.description")
        SettingsRow {
            //% "Format"
            label: qsTrId("gamehq.settings.capture.image.format")
            SettingsCombo {
                id: formatCombo
                configKey: "capture.screenshot_format"
                defaultValue: "png"
                options: [
                    //% "PNG (lossless)"
                    { label: qsTrId("gamehq.settings.capture.image.png"), value: "png" },
                    //% "JPEG (smaller files)"
                    { label: qsTrId("gamehq.settings.capture.image.jpeg"), value: "jpg" }
                ]
            }
        }
        SettingsRow {
            //% "JPEG quality"
            label: qsTrId("gamehq.settings.capture.image.jpeg_quality")
            visible: formatCombo.currentIndex >= 0
                     && formatCombo.options[formatCombo.currentIndex].value === "jpg"
            SettingsCombo {
                configKey: "capture.jpeg_quality"
                defaultValue: 90
                options: [
                    { label: "70%", value: 70 }, { label: "80%", value: 80 },
                    { label: "90%", value: 90 }, { label: "100%", value: 100 }
                ]
            }
        }
    }

    SettingsSection {
        //% "Feedback"
        eyebrow: qsTrId("gamehq.settings.capture.feedback.eyebrow")
        //% "After a screenshot"
        title: qsTrId("gamehq.settings.capture.feedback.title")
        //% "Combined with the master switches on the Notifications & Sound page."
        description: qsTrId("gamehq.settings.capture.feedback.description")
        SettingsRow {
            //% "Screenshot sound"
            label: qsTrId("gamehq.settings.capture.feedback.sound")
            SettingsToggle { configKey: "capture.screenshot_sound"; defaultValue: true }
        }
        SettingsRow {
            //% "Screenshot notification"
            label: qsTrId("gamehq.settings.capture.feedback.notification")
            SettingsToggle { configKey: "capture.screenshot_notify"; defaultValue: true }
        }
    }

    SettingsSection {
        //% "Storage"
        eyebrow: qsTrId("gamehq.settings.capture.storage.eyebrow")
        //% "Where captures are saved"
        title: qsTrId("gamehq.settings.capture.storage.title")
        //% "Changing a location never moves or deletes existing media."
        description: qsTrId("gamehq.settings.capture.storage.description")

        SettingsPathRow {
            //% "Screenshots"
            label: qsTrId("gamehq.settings.capture.storage.screenshots")
            path: app.screenshotsRoot
            showChange: true
            showReset: app.screenshotsRoot !== app.capturesRoot
            onChangeRequested: screenshotFolderDialog.open()
            onOpenRequested: root.openFolder(app.screenshotsRoot)
            onResetRequested: root.finishLocationChange(app.resetCaptureRoot("screenshots"))
        }

        SettingsPathRow {
            //% "Replay clips"
            label: qsTrId("gamehq.settings.capture.storage.replay_clips")
            path: app.clipsRoot
            showChange: true
            showReset: app.clipsRoot !== app.capturesRoot
            showDivider: false
            onChangeRequested: clipFolderDialog.open()
            onOpenRequested: root.openFolder(app.clipsRoot)
            onResetRequested: root.finishLocationChange(app.resetCaptureRoot("clips"))
        }

        Text {
            visible: root.locationError.length > 0
            text: root.locationError
            color: Theme.danger
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    FolderDialog {
        id: screenshotFolderDialog
        //% "Choose the screenshots folder"
        title: qsTrId("gamehq.settings.capture.storage.choose_screenshots")
        onAccepted: root.finishLocationChange(app.setCaptureRoot("screenshots", selectedFolder))
    }

    FolderDialog {
        id: clipFolderDialog
        //% "Choose the clips folder"
        title: qsTrId("gamehq.settings.capture.storage.choose_clips")
        onAccepted: root.finishLocationChange(app.setCaptureRoot("clips", selectedFolder))
    }
}
