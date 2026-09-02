import QtQuick
import GameHQ
import "../components"

SettingsPage {
    //% "Notifications & Sound"
    pageTitle: qsTrId("gamehq.settings.feedback.title")
    //% "Choose how GameHQ confirms captures, clips, and navigation."
    pageDescription: qsTrId("gamehq.settings.feedback.description")

    SettingsSection {
        //% "Visual feedback"
        eyebrow: qsTrId("gamehq.settings.feedback.visual.eyebrow")
        //% "Notifications"
        title: qsTrId("gamehq.settings.feedback.visual.title")
        //% "Control the result cards shown after capture and replay actions."
        description: qsTrId("gamehq.settings.feedback.visual.description")
        SettingsRow {
            icon: "\u25A3"
            //% "Show notifications"
            label: qsTrId("gamehq.settings.feedback.visual.enabled.label")
            //% "Master switch for capture and replay result cards."
            description: qsTrId("gamehq.settings.feedback.visual.enabled.description")
            SettingsToggle { configKey: "notifications.enabled"; defaultValue: true }
        }
        SettingsRow {
            icon: "\u25A3"
            //% "Screenshot captured"
            label: qsTrId("gamehq.settings.feedback.visual.screenshot")
            SettingsToggle { configKey: "capture.screenshot_notify"; defaultValue: true }
        }
        SettingsRow {
            icon: "\u21BA"
            //% "Replay saved"
            label: qsTrId("gamehq.settings.feedback.visual.replay")
            showDivider: false
            SettingsToggle { configKey: "replay.clip_notify"; defaultValue: true }
        }
    }

    SettingsSection {
        //% "Audio feedback"
        eyebrow: qsTrId("gamehq.settings.feedback.audio.eyebrow")
        //% "Sound"
        title: qsTrId("gamehq.settings.feedback.audio.title")
        //% "Set the master sound switch, volume, and event-specific feedback."
        description: qsTrId("gamehq.settings.feedback.audio.description")
        SettingsRow {
            icon: "\u266B"
            //% "UI sounds"
            label: qsTrId("gamehq.settings.feedback.audio.enabled.label")
            //% "Play navigation and action feedback sounds."
            description: qsTrId("gamehq.settings.feedback.audio.enabled.description")
            SettingsToggle { configKey: "sounds.enabled"; defaultValue: true }
        }
        SettingsRow {
            //% "Volume"
            label: qsTrId("gamehq.settings.feedback.audio.volume")
            SettingsSlider {
                configKey: "sounds.volume"
                defaultValue: 80
                from: 0
                to: 100
                stepSize: 5
            }
        }
        SettingsRow {
            //% "Screenshot sound"
            label: qsTrId("gamehq.settings.feedback.audio.screenshot")
            SettingsToggle { configKey: "capture.screenshot_sound"; defaultValue: true }
        }
        SettingsRow {
            //% "Replay saved sound"
            label: qsTrId("gamehq.settings.feedback.audio.replay")
            showDivider: false
            SettingsToggle { configKey: "replay.clip_sound"; defaultValue: true }
        }
    }

    SettingsSection {
        //% "Preview"
        eyebrow: qsTrId("gamehq.settings.feedback.preview.eyebrow")
        //% "Test the current feedback settings"
        title: qsTrId("gamehq.settings.feedback.preview.title")
        variant: "compact"
        //% "Confirm notifications and sound without creating a capture."
        description: qsTrId("gamehq.settings.feedback.preview.description")
        SettingsRow {
            //% "Preview feedback"
            label: qsTrId("gamehq.settings.feedback.preview.label")
            showDivider: false
            controlWidth: Theme.s48 * 6
            AccentButton {
                //% "Show test notification"
                label: qsTrId("gamehq.settings.feedback.preview.notification")
                quiet: true
                onClicked: {
                    //% "%1 notification"
                    const title = qsTrId("gamehq.settings.feedback.preview.notification_title").arg(Brand.name)
                    //% "Notifications are working."
                    const body = qsTrId("gamehq.settings.feedback.preview.notification_body")
                    notifications.post(title, body, "", "info")
                }
            }
            AccentButton {
                //% "Play test sound"
                label: qsTrId("gamehq.settings.feedback.preview.sound")
                primary: true
                onClicked: sounds.play("confirm")
            }
        }
    }
}
