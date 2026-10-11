import QtQuick
import GameHQ

// Opt-in beta update channel, shown in both General and About. Off by default;
// UpdateService offers vX.Y.Z-betaN pre-releases only while it is on.
SettingsRow {
    //% "Include beta versions"
    label: qsTrId("gamehq.update.include_betas.label")
    //% "Also offer test pre-releases. They get new fixes sooner but may be less stable."
    description: qsTrId("gamehq.update.include_betas.description")
    SettingsToggle { configKey: "updates.include_betas"; defaultValue: false }
}
