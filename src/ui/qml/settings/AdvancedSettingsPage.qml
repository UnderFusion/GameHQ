import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import GameHQ
import "../components"

SettingsPage {
    id: root
    //% "Advanced"
    pageTitle: qsTrId("gamehq.settings.category.advanced")
    //% "Review system health, open diagnostic resources, and recover settings."
    pageDescription: qsTrId("gamehq.settings.advanced.description")

    signal restoreAllRequested()
    signal restoreCategoryRequested(string category)
    signal restoreInputRequested()
    signal importPortableRequested(url folderUrl)

    function hdrDetailItems() {
        return String(app.hdrDetailText || "").split(/\r?\n/)
            .filter(function(line) { return line.trim().length > 0 })
            .map(function(line) {
                const separator = line.indexOf(":")
                return separator > 0
                    ? { label: line.slice(0, separator).trim(),
                        value: line.slice(separator + 1).trim() }
                    : { label: "", value: line.trim() }
            })
    }

    SettingsSection {
        //% "Overview"
        eyebrow: qsTrId("gamehq.settings.advanced.overview.eyebrow")
        //% "System status"
        title: qsTrId("gamehq.settings.advanced.overview.title")
        //% "A concise view of the environment GameHQ is currently using."
        description: qsTrId("gamehq.settings.advanced.overview.description")
        SettingsStatusStrip {
            items: [
                {
                  //% "System"
                  label: qsTrId("gamehq.settings.advanced.overview.system"),
                  //% "Ready"
                  value: qsTrId("gamehq.settings.advanced.status.ready"),
                  detail: app.portableMode
                      //% "Portable profile"
                      ? qsTrId("gamehq.settings.advanced.profile.portable")
                      //% "Installed profile"
                      : qsTrId("gamehq.settings.advanced.profile.installed")
                },
                {
                  //% "Capture"
                  label: qsTrId("gamehq.settings.category.capture"),
                  value: app.hdrDisplayActive
                      //% "HDR active"
                      ? qsTrId("gamehq.settings.advanced.hdr.active")
                      //% "HDR inactive"
                      : qsTrId("gamehq.settings.advanced.hdr.inactive"),
                  detail: app.hdrStatusText,
                  tone: app.hdrStatusText.toLowerCase().indexOf("unavailable") >= 0
                        ? "warning" : app.hdrDisplayActive ? "accent" : "danger"
                },
                {
                  //% "Storage"
                  label: qsTrId("gamehq.settings.advanced.overview.storage"),
                  //% "Healthy"
                  value: qsTrId("gamehq.settings.advanced.status.healthy"),
                  //% "Managed folders available"
                  detail: qsTrId("gamehq.settings.advanced.storage.available")
                },
                {
                  //% "Version"
                  label: qsTrId("gamehq.settings.advanced.overview.version"),
                  value: app.version,
                  //% "Current installation"
                  detail: qsTrId("gamehq.settings.advanced.version.current")
                }
            ]
        }
    }

    SettingsSection {
        //% "Resources"
        eyebrow: qsTrId("gamehq.settings.advanced.resources.eyebrow")
        //% "Locations"
        title: qsTrId("gamehq.settings.advanced.resources.title")
        //% "Open GameHQ-owned folders used for logs, configuration, database, and support data."
        description: qsTrId("gamehq.settings.advanced.resources.description")
        SettingsPathRow {
            //% "Logs folder"
            label: qsTrId("gamehq.settings.advanced.resources.logs")
            path: app.logsRoot
            onOpenRequested: app.openLogsFolder()
        }
        SettingsPathRow {
            //% "Data folder"
            label: qsTrId("gamehq.settings.advanced.resources.data")
            path: app.dataRoot
            showDivider: false
            onOpenRequested: app.openDataFolder()
        }
    }

    SettingsSection {
        //% "Diagnostics"
        eyebrow: qsTrId("gamehq.settings.advanced.diagnostics.eyebrow")
        //% "Tools"
        title: qsTrId("gamehq.settings.advanced.diagnostics.title")
        //% "Collect support information or refresh hardware status without changing settings."
        description: qsTrId("gamehq.settings.advanced.diagnostics.description")
        GridLayout {
            Layout.fillWidth: true
            columns: width < 720 ? 1 : 2
            columnSpacing: Theme.s8
            rowSpacing: Theme.s8
            SettingsActionTile {
                icon: "\u2398"
                //% "Copy diagnostic summary"
                title: qsTrId("gamehq.settings.advanced.diagnostics.copy.title")
                //% "Version, profile mode, and managed paths."
                description: qsTrId("gamehq.settings.advanced.diagnostics.copy.description")
                onClicked: { app.copyDiagnosticSummary(); sounds.play("confirm") }
            }
            SettingsActionTile {
                icon: "\u21BB"
                //% "Refresh display status"
                title: qsTrId("gamehq.settings.advanced.diagnostics.refresh.title")
                //% "Recheck HDR and capture capabilities."
                description: qsTrId("gamehq.settings.advanced.diagnostics.refresh.description")
                onClicked: app.refreshHdrStatus()
            }
            SettingsActionTile {
                visible: !app.portableMode
                icon: "\u21E5"
                //% "Import portable profile"
                title: qsTrId("gamehq.settings.advanced.diagnostics.import.title")
                //% "Validate, stage, and import a fresh portable profile."
                description: qsTrId("gamehq.settings.advanced.diagnostics.import.description")
                onClicked: portableFolderDialog.open()
            }
        }
    }

    SettingsSection {
        //% "Display capture"
        eyebrow: qsTrId("gamehq.settings.advanced.hdr.eyebrow")
        //% "HDR details"
        title: qsTrId("gamehq.settings.advanced.hdr.title")
        status: app.hdrStatusText
        statusColor: app.hdrDisplayActive ? Theme.accent : Theme.danger
        //% "Technical adapter and fallback details are available when troubleshooting capture output."
        description: qsTrId("gamehq.settings.advanced.hdr.description")
        SettingsDisclosure {
            //% "Technical HDR details"
            label: qsTrId("gamehq.settings.advanced.hdr.disclosure")
            Repeater {
                model: root.hdrDetailItems()
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.s4
                    spacing: Theme.s12

                    Text {
                        text: "\u2022"
                        color: Theme.accent
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                        Layout.alignment: Qt.AlignTop
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.s4
                        Text {
                            visible: modelData.label.length > 0
                            text: modelData.label
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontCaption
                            font.weight: Font.DemiBold
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        Text {
                            text: modelData.value
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }

    SettingsSection {
        //% "Recovery"
        eyebrow: qsTrId("gamehq.settings.advanced.recovery.eyebrow")
        //% "Restore"
        title: qsTrId("gamehq.settings.advanced.recovery.title")
        variant: "compact"
        //% "Restoring settings never deletes captures, favorites, watched media, or database records."
        description: qsTrId("gamehq.settings.advanced.recovery.description")
        SettingsDisclosure {
            //% "Restore options"
            label: qsTrId("gamehq.settings.advanced.recovery.options")
            SettingsRow {
                //% "Restore one category"
                label: qsTrId("gamehq.settings.advanced.recovery.category.label")
                //% "Return only the selected category to its defaults."
                description: qsTrId("gamehq.settings.advanced.recovery.category.description")
                controlWidth: Theme.s48 * 9
                AccentButton {
                    //% "General"
                    label: qsTrId("gamehq.settings.category.general")
                    quiet: true
                    onClicked: root.restoreCategoryRequested("General")
                }
                AccentButton {
                    //% "Capture"
                    label: qsTrId("gamehq.settings.category.capture")
                    quiet: true
                    onClicked: root.restoreCategoryRequested("Capture")
                }
                AccentButton {
                    //% "Replay"
                    label: qsTrId("gamehq.settings.category.replay")
                    quiet: true
                    onClicked: root.restoreCategoryRequested("Replay")
                }
                AccentButton {
                    //% "Feedback"
                    label: qsTrId("gamehq.settings.advanced.recovery.feedback")
                    quiet: true
                    onClicked: root.restoreCategoryRequested("Notifications & Sound")
                }
            }
            SettingsRow {
                //% "Restore input bindings"
                label: qsTrId("gamehq.settings.advanced.recovery.input.label")
                //% "Return controller, keyboard, and mouse overrides to built-in defaults."
                description: qsTrId("gamehq.settings.advanced.recovery.input.description")
                AccentButton {
                    //% "Restore input"
                    label: qsTrId("gamehq.settings.advanced.recovery.input.action")
                    quiet: true
                    onClicked: root.restoreInputRequested()
                }
            }
            SettingsRow {
                tone: "danger"
                //% "Restore all settings"
                label: qsTrId("gamehq.settings.advanced.recovery.all.label")
                //% "Return every configuration category to its default values."
                description: qsTrId("gamehq.settings.advanced.recovery.all.description")
                showDivider: false
                AccentButton {
                    //% "Restore all"
                    label: qsTrId("gamehq.settings.advanced.recovery.all.action")
                    quiet: true
                    labelColor: Theme.danger
                    borderColor: Theme.danger
                    onClicked: root.restoreAllRequested()
                }
            }
        }
    }

    FolderDialog {
        id: portableFolderDialog
        //% "Select the GameHQ portable folder"
        title: qsTrId("gamehq.settings.advanced.portable_folder.title")
        onAccepted: root.importPortableRequested(selectedFolder)
    }
}
