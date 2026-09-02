import QtQuick
import QtQuick.Dialogs
import GameHQ
import "../components"

SettingsPage {
    id: root
    //% "Library"
    pageTitle: qsTrId("gamehq.settings.library.title")
    //% "Review every folder GameHQ manages or scans for media."
    pageDescription: qsTrId("gamehq.settings.library.description")
    pageAction: Component {
        AccentButton {
        //% "Rescan now"
        label: qsTrId("gamehq.settings.library.rescan")
            quiet: true
            onClicked: {
                app.rescan()
                sounds.play("confirm")
            }
        }
    }

    function openFolder(path) {
        Qt.openUrlExternally("file:///" + path.replace(/\\/g, "/"))
    }

    SettingsSection {
        //% "Storage"
        eyebrow: qsTrId("gamehq.settings.library.storage.eyebrow")
        //% "Managed locations"
        title: qsTrId("gamehq.settings.library.storage.title")
        //% "2 active"
        badge: qsTrId("gamehq.settings.library.storage.active_count")
        //% "Current output folders and earlier roots remain scanned so past media stays visible."
        description: qsTrId("gamehq.settings.library.storage.description")

        SettingsPathRow {
            //% "Screenshots"
            label: qsTrId("gamehq.settings.library.storage.screenshots")
            path: app.screenshotsRoot
            onOpenRequested: root.openFolder(app.screenshotsRoot)
        }
        SettingsPathRow {
            //% "Replay clips"
            label: qsTrId("gamehq.settings.library.storage.replay_clips")
            path: app.clipsRoot
            onOpenRequested: root.openFolder(app.clipsRoot)
        }
        Repeater {
            model: app.managedRoots.filter(function(path) {
                return path !== app.screenshotsRoot && path !== app.clipsRoot
                       && path !== app.capturesRoot
            })
            delegate: SettingsPathRow {
                required property string modelData
                //% "Previous location"
                label: qsTrId("gamehq.settings.library.storage.previous_location")
                path: modelData
                showDivider: index < app.managedRoots.length - 1
                onOpenRequested: root.openFolder(modelData)
            }
        }
    }

    SettingsSection {
        //% "Imports"
        eyebrow: qsTrId("gamehq.settings.library.imports.eyebrow")
        //% "Watched folders"
        title: qsTrId("gamehq.settings.library.imports.title")
        //% "%n watched folder(s)"
        badge: qsTrId("gamehq.settings.library.imports.count", app.watchedFolders.length)
        //% "External folders are scanned read-only and never become GameHQ output locations."
        description: qsTrId("gamehq.settings.library.imports.description")

        SettingsEmptyState {
            visible: app.watchedFolders.length === 0
            icon: "\uFF0B"
            //% "No watched folders yet"
            title: qsTrId("gamehq.settings.library.imports.empty.title")
            //% "Add folders created by Steam, OBS, Xbox Game Bar, or another capture tool."
            description: qsTrId("gamehq.settings.library.imports.empty.description")
            AccentButton {
                //% "Add watched folder"
                label: qsTrId("gamehq.settings.library.imports.add")
                primary: true
                onClicked: watchedFolderDialog.open()
            }
        }

        Repeater {
            model: app.watchedFolders
            delegate: SettingsRow {
                required property string modelData
                icon: "\u25A4"
                //% "Watched folder"
                label: qsTrId("gamehq.settings.library.imports.folder")
                description: modelData
                controlWidth: Theme.s48 * 5
                AccentButton {
                    //% "Open"
                    label: qsTrId("gamehq.common.action.open")
                    quiet: true
                    onClicked: root.openFolder(modelData)
                }
                AccentButton {
                    //% "Remove"
                    label: qsTrId("gamehq.action.remove")
                    quiet: true
                    labelColor: Theme.danger
                    borderColor: Theme.danger
                    onClicked: app.removeWatchedFolder(modelData)
                }
            }
        }

        AccentButton {
            visible: app.watchedFolders.length > 0
            //% "Add watched folder"
            label: qsTrId("gamehq.settings.library.imports.add")
            quiet: true
            onClicked: watchedFolderDialog.open()
        }
    }

    SettingsSection {
        //% "Last scan"
        eyebrow: qsTrId("gamehq.settings.library.scan.eyebrow")
        title: {
            if (!app.lastScanAvailable) {
                //% "Not scanned this session"
                return qsTrId("gamehq.settings.library.scan.not_scanned")
            }
            if (app.lastScanAdded === 0) {
                //% "Library is up to date"
                return qsTrId("gamehq.settings.library.scan.up_to_date")
            }
            //% "%n new capture(s) added"
            return qsTrId("gamehq.settings.library.scan.added", app.lastScanAdded)
        }
        variant: "compact"
        //% "Rescan checks current, previous, and watched locations for media missing from the library."
        description: qsTrId("gamehq.settings.library.scan.description")
    }

    FolderDialog {
        id: watchedFolderDialog
        //% "Choose a folder to watch"
        title: qsTrId("gamehq.library.folder_dialog.title")
        onAccepted: app.addWatchedFolder(selectedFolder)
    }
}
