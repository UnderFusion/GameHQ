import QtQuick
import GameHQ
import "../components"

SettingsPage {
    //% "Replay"
    pageTitle: qsTrId("gamehq.settings.replay.title")
    //% "Manage the rolling buffer used for instant replay clips."
    pageDescription: qsTrId("gamehq.settings.replay.description")

    SettingsSection {
        //% "Current status"
        eyebrow: qsTrId("gamehq.settings.replay.status.eyebrow")
        //% "Replay buffer"
        title: qsTrId("gamehq.settings.replay.status.title")
        variant: "status"
        status: {
            if (app.replayBufferActive) {
                //% "Recording"
                return qsTrId("gamehq.settings.replay.status.recording")
            }
            //% "Idle"
            return qsTrId("gamehq.settings.replay.status.idle")
        }
        description: {
            if (app.replayBufferActive) {
                //% "Recording %1; the temporary ring is written only when you save a replay."
                return qsTrId("gamehq.settings.replay.status.active_description").arg(app.replayBufferGame)
            }
            //% "Not recording. The buffer arms automatically when an eligible game is active."
            return qsTrId("gamehq.settings.replay.status.idle_description")
        }
    }

    SettingsSection {
        //% "Buffer"
        eyebrow: qsTrId("gamehq.settings.replay.buffer.eyebrow")
        //% "Automatic recording"
        title: qsTrId("gamehq.settings.replay.buffer.title")
        //% "Recording changes restart an active buffer so new values apply immediately."
        description: qsTrId("gamehq.settings.replay.buffer.description")
        SettingsRow {
            //% "Automatic buffer"
            label: qsTrId("gamehq.settings.replay.buffer.enabled.label")
            //% "Record a rolling buffer whenever an eligible game is active."
            description: qsTrId("gamehq.settings.replay.buffer.enabled.description")
            SettingsToggle { configKey: "replay.auto"; defaultValue: true }
        }
        SettingsRow {
            //% "Replay length"
            label: qsTrId("gamehq.settings.replay.buffer.length")
            showDivider: false
            SettingsCombo {
                configKey: "replay.length_seconds"; defaultValue: 300
                options: [
                    //% "%n second(s)"
                    { label: qsTrId("gamehq.duration.seconds", 30), value: 30 },
                    //% "%n minute(s)"
                    { label: qsTrId("gamehq.duration.minutes", 1), value: 60 },
                    //% "%n minute(s)"
                    { label: qsTrId("gamehq.duration.minutes", 3), value: 180 },
                    //% "%n minute(s)"
                    { label: qsTrId("gamehq.duration.minutes", 5), value: 300 },
                    //% "%n minute(s)"
                    { label: qsTrId("gamehq.duration.minutes", 10), value: 600 },
                    //% "%n minute(s)"
                    { label: qsTrId("gamehq.duration.minutes", 15), value: 900 }
                ]
            }
        }
    }

    SettingsSection {
        //% "Encoding"
        eyebrow: qsTrId("gamehq.settings.replay.encoding.eyebrow")
        //% "Recording quality"
        title: qsTrId("gamehq.settings.replay.encoding.title")
        //% "Balance motion detail, resolution, storage use, and encoder load."
        description: qsTrId("gamehq.settings.replay.encoding.description")
        SettingsRow {
            //% "Frame rate"
            label: qsTrId("gamehq.settings.replay.encoding.frame_rate")
            SettingsCombo {
                configKey: "replay.fps"; defaultValue: 30
                options: [
                    //% "%1 fps"
                    { label: qsTrId("gamehq.format.fps").arg(languageManager.formatInteger(30)), value: 30 },
                    //% "%1 fps"
                    { label: qsTrId("gamehq.format.fps").arg(languageManager.formatInteger(60)), value: 60 }
                ]
            }
        }
        SettingsRow {
            //% "Resolution"
            label: qsTrId("gamehq.settings.replay.encoding.resolution")
            SettingsCombo {
                configKey: "replay.resolution"; defaultValue: "1920x1080"
                options: [
                    { label: "720p", value: "1280x720" },
                    { label: "1080p", value: "1920x1080" },
                    { label: "4K", value: "3840x2160" }
                ]
            }
        }
        SettingsRow {
            //% "Video bitrate"
            label: qsTrId("gamehq.settings.replay.encoding.bitrate")
            //% "Higher values improve motion detail but use more storage and encoder bandwidth."
            description: qsTrId("gamehq.settings.replay.encoding.bitrate_description")
            SettingsCombo {
                configKey: "replay.bitrate_mbps"; defaultValue: 14
                options: [
                    //% "%1 Mbps"
                    { label: qsTrId("gamehq.format.megabits_per_second").arg(languageManager.formatInteger(8)), value: 8 },
                    //% "%1 Mbps"
                    { label: qsTrId("gamehq.format.megabits_per_second").arg(languageManager.formatInteger(14)), value: 14 },
                    //% "%1 Mbps"
                    { label: qsTrId("gamehq.format.megabits_per_second").arg(languageManager.formatInteger(20)), value: 20 },
                    //% "%1 Mbps"
                    { label: qsTrId("gamehq.format.megabits_per_second").arg(languageManager.formatInteger(35)), value: 35 }
                ]
            }
        }
        SettingsRow {
            id: audioSourceRow
            // "auto" and the legacy on/off switch resolve to a concrete choice
            // in C++, so the dropdown always shows what will be recorded.
            property string selection: app.audioSourceSelection()
            readonly property bool gameAudioSupported: app.gameAudioSupported()
            //% "Audio source"
            label: qsTrId("gamehq.settings.replay.encoding.audio_source.label")
            description: {
                if (selection === "off") {
                    //% "Replay clips are saved without sound."
                    return qsTrId("gamehq.settings.replay.encoding.audio_source.off_description")
                }
                if (!gameAudioSupported) {
                    //% "Game audio needs Windows 11. Desktop audio records everything on the default Windows output."
                    return qsTrId("gamehq.settings.replay.encoding.audio_source.unsupported_description")
                }
                if (selection === "desktop") {
                    //% "Records everything on the default Windows output, including chat and music apps."
                    return qsTrId("gamehq.settings.replay.encoding.audio_source.desktop_description")
                }
                //% "Records only the current game, whichever output device it plays on."
                return qsTrId("gamehq.settings.replay.encoding.audio_source.game_description")
            }
            showDivider: false
            SettingsCombo {
                id: audioSourceCombo
                defaultValue: audioSourceRow.selection
                options: [
                    {
                        label: audioSourceRow.gameAudioSupported
                            //% "Game audio"
                            ? qsTrId("gamehq.settings.replay.encoding.audio_source.game")
                            //% "Game audio (needs Windows 11)"
                            : qsTrId("gamehq.settings.replay.encoding.audio_source.game_unsupported"),
                        value: "game"
                    },
                    //% "Desktop audio"
                    { label: qsTrId("gamehq.settings.replay.encoding.audio_source.desktop"), value: "desktop" },
                    //% "Off"
                    { label: qsTrId("gamehq.settings.replay.encoding.audio_source.off"), value: "off" }
                ]
                onValueCommitted: function(value) { app.setConfig("audio.source", value) }

                Connections {
                    target: app
                    function onConfigChanged(key, value) {
                        if (key === "audio.source" || key === "audio.enabled")
                            audioSourceRow.selection = app.audioSourceSelection()
                    }
                    function onConfigGroupReset(prefix) {
                        audioSourceRow.selection = app.audioSourceSelection()
                    }
                }
            }
        }
    }

    SettingsSection {
        //% "Feedback"
        eyebrow: qsTrId("gamehq.settings.replay.feedback.eyebrow")
        //% "After saving a clip"
        title: qsTrId("gamehq.settings.replay.feedback.title")
        //% "Saved replays go to %1. Failures always notify you."
        description: qsTrId("gamehq.settings.replay.feedback.description").arg(app.clipsRoot)
        SettingsRow {
            //% "Clip saved sound"
            label: qsTrId("gamehq.settings.replay.feedback.sound")
            SettingsToggle { configKey: "replay.clip_sound"; defaultValue: true }
        }
        SettingsRow {
            //% "Clip saved notification"
            label: qsTrId("gamehq.settings.replay.feedback.notification")
            showDivider: false
            SettingsToggle { configKey: "replay.clip_notify"; defaultValue: true }
        }
    }
}
