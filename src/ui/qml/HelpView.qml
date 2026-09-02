import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as QC
import GameHQ
import "components"

// Help page — keyboard shortcuts, controller bindings, and quick feature reference.
Item {
    id: root

    function resetScroll() {
        flick.contentY = 0
    }

    function scrollBy(direction) {
        const maximum = Math.max(0, flick.contentHeight - flick.height)
        flick.contentY = Math.max(0, Math.min(maximum,
            flick.contentY + direction * Math.max(80, flick.height * 0.28)))
    }

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: width
        contentHeight: helpColumn.implicitHeight + Theme.s16
        flickableDirection: Flickable.VerticalFlick

        QC.ScrollBar.vertical: AppScrollBar {}

        ColumnLayout {
            id: helpColumn
            width: Math.max(0, flick.width - Theme.s16)
            spacing: Theme.s24

    // ── Keyboard shortcuts ──
    ColumnLayout {
        spacing: Theme.s8
        Text {
            //% "Keyboard shortcuts"
            text: qsTrId("gamehq.help.keyboard_shortcuts").toUpperCase()
            color: Theme.textFaint
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            font.letterSpacing: Theme.letterSpacingWide
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: keysCol.implicitHeight + Theme.s16 * 2
            radius: Theme.radiusM
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke

            ColumnLayout {
                id: keysCol
                anchors.fill: parent
                anchors.margins: Theme.s16
                spacing: Theme.s8

                Repeater {
                    model: [
                        {
                            binding: "Alt+Shift+G",
                            //% "Open / close overlay"
                            act: qsTrId("gamehq.help.keyboard.open_overlay")
                        },
                        {
                            binding: "Ctrl+Shift+S",
                            //% "Take screenshot"
                            act: qsTrId("gamehq.help.keyboard.take_screenshot")
                        },
                        {
                            binding: "Ctrl+Shift+E",
                            //% "Save last N seconds as clip"
                            act: qsTrId("gamehq.help.keyboard.save_replay")
                        },
                        {
                            binding: "Enter",
                            //% "Open selected capture"
                            act: qsTrId("gamehq.help.keyboard.open_capture")
                        },
                        {
                            //% "Select mode"
                            binding: qsTrId("gamehq.help.binding.select_mode"),
                            //% "Enter / Space toggles, Ctrl+A selects all, Delete removes selected"
                            act: qsTrId("gamehq.help.keyboard.select_mode")
                        },
                        {
                            binding: "F",
                            //% "Favorite / unfavorite selected"
                            act: qsTrId("gamehq.help.keyboard.toggle_favorite")
                        },
                        {
                            binding: "E",
                            //% "Show selected in Explorer"
                            act: qsTrId("gamehq.help.keyboard.show_in_explorer")
                        },
                        {
                            binding: "W / A / S / D",
                            //% "Navigate gallery grid"
                            act: qsTrId("gamehq.help.navigate_gallery")
                        }
                    ]
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        Rectangle {
                            Layout.minimumWidth: 160
                            implicitWidth: bindingLabel.implicitWidth + Theme.s8 * 2
                            implicitHeight: 28
                            radius: Theme.radiusS
                            color: Theme.surfaceAlt
                            Text {
                                id: bindingLabel
                                anchors.centerIn: parent
                                text: modelData.binding
                                color: Theme.accent
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.DemiBold
                            }
                        }
                        Text {
                            text: modelData.act
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }

    // ── Controller (DualSense / gamepad) ──
    ColumnLayout {
        spacing: Theme.s8
        Layout.fillWidth: true
        Text {
            //% "DualSense / gamepad"
            text: qsTrId("gamehq.help.gamepad_shortcuts").toUpperCase()
            color: Theme.textFaint
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            font.letterSpacing: Theme.letterSpacingWide
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: ctrlCol.implicitHeight + Theme.s16 * 2
            radius: Theme.radiusM
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke

            ColumnLayout {
                id: ctrlCol
                anchors.fill: parent
                anchors.margins: Theme.s16
                spacing: Theme.s8

                Repeater {
                    model: [
                        {
                            //% "System Share (tap)"
                            binding: qsTrId("gamehq.help.gamepad.system_share_tap"),
                            //% "Take screenshot; distinct from Xbox View / Back"
                            act: qsTrId("gamehq.help.gamepad.system_share_action")
                        },
                        {
                            //% "Share (hold)"
                            binding: qsTrId("gamehq.help.gamepad.share_hold"),
                            //% "Save replay clip; hold consumes the tap"
                            act: qsTrId("gamehq.help.gamepad.share_hold_action")
                        },
                        {
                            //% "Share (2× / 3×)"
                            binding: qsTrId("gamehq.help.gamepad.share_multi_tap"),
                            //% "Exact double/triple tap; lower counts wait only for the configured interval"
                            act: qsTrId("gamehq.help.gamepad.share_multi_tap_action")
                        },
                        {
                            //% "Button combination"
                            binding: qsTrId("gamehq.help.gamepad.button_combination"),
                            //% "Press the ordered pair within the configured combination window"
                            act: qsTrId("gamehq.help.gamepad.button_combination_action")
                        },
                        {
                            //% "PS button"
                            binding: qsTrId("gamehq.help.gamepad.ps_button"),
                            //% "Open / close overlay"
                            act: qsTrId("gamehq.help.keyboard.open_overlay")
                        },
                        {
                            binding: "L1 / R1",
                            //% "Switch panel: sidebar ↔ grid (app) / flip captures (overlay)"
                            act: qsTrId("gamehq.help.gamepad.switch_panel")
                        },
                        {
                            //% "D-pad / Left Stick"
                            binding: qsTrId("gamehq.help.gamepad.navigation_controls"),
                            //% "Navigate gallery grid"
                            act: qsTrId("gamehq.help.navigate_gallery")
                        },
                        {
                            //% "Cross"
                            binding: qsTrId("gamehq.help.gamepad.cross"),
                            //% "Open selected capture / confirm"
                            act: qsTrId("gamehq.help.gamepad.cross_action")
                        },
                        {
                            //% "Circle"
                            binding: qsTrId("gamehq.help.gamepad.circle"),
                            //% "Back / close overlay"
                            act: qsTrId("gamehq.help.gamepad.circle_action")
                        },
                        {
                            //% "Square"
                            binding: qsTrId("gamehq.help.gamepad.square"),
                            //% "Action menu (Show in folder / Delete)"
                            act: qsTrId("gamehq.help.gamepad.square_action")
                        },
                        {
                            //% "Triangle"
                            binding: qsTrId("gamehq.help.gamepad.triangle"),
                            //% "Favorite / unfavorite selected"
                            act: qsTrId("gamehq.help.keyboard.toggle_favorite")
                        },
                        {
                            //% "Select mode"
                            binding: qsTrId("gamehq.help.binding.select_mode"),
                            //% "Cross toggles, Triangle selects all, Square deletes, Circle exits"
                            act: qsTrId("gamehq.help.gamepad.select_mode")
                        }
                    ]
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        Rectangle {
                            Layout.minimumWidth: 130
                            implicitWidth: ctrlLabel.implicitWidth + Theme.s8 * 2
                            implicitHeight: 28
                            radius: Theme.radiusS
                            color: Theme.surfaceAlt
                            Text {
                                id: ctrlLabel
                                anchors.centerIn: parent
                                text: modelData.binding
                                color: Theme.accent
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.DemiBold
                            }
                        }
                        Text {
                            text: modelData.act
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                            Layout.fillWidth: true
                        }
                    }
                }
                TextLink {
                    //% "Open the controller compatibility guide"
                    label: qsTrId("gamehq.help.open_controller_guide")
                    suffix: "↗"
                    onClicked: Qt.openUrlExternally(Brand.repositoryUrl + "/blob/dev/docs/controller-compatibility.md")
                }
            }
        }
    }

    // ── Features quick reference ──
    ColumnLayout {
        spacing: Theme.s8
        Layout.fillWidth: true
        Text {
            //% "Features"
            text: qsTrId("gamehq.help.features").toUpperCase()
            color: Theme.textFaint
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            font.letterSpacing: Theme.letterSpacingWide
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: featCol.implicitHeight + Theme.s16 * 2
            radius: Theme.radiusM
            color: Theme.surface
            border.width: 1
            border.color: Theme.stroke

            ColumnLayout {
                id: featCol
                anchors.fill: parent
                anchors.margins: Theme.s16
                spacing: Theme.s8

                Repeater {
                    model: [
                        {
                            //% "Replay buffer"
                            heading: qsTrId("gamehq.help.feature.replay.title"),
                            //% "Always-on auto-armed. Records in the background while a game is in focus. Hold Share (or Ctrl+Shift+E) to save the last few seconds as a clip. Turn always-on recording on or off in Settings → Replay."
                            desc: qsTrId("gamehq.help.feature.replay.description")
                        },
                        {
                            //% "Screenshots"
                            heading: qsTrId("gamehq.help.feature.screenshots.title"),
                            //% "GDI grab of the active game window. PNG saved to your captures folder with instant shutter feedback."
                            desc: qsTrId("gamehq.help.feature.screenshots.description")
                        },
                        {
                            //% "Gallery"
                            heading: qsTrId("gamehq.help.feature.gallery.title"),
                            //% "All captures in one grid — filter by category or game. Grid navigation works with keyboard, mouse, and controller."
                            desc: qsTrId("gamehq.help.feature.gallery.description")
                        },
                        {
                            //% "Overlay"
                            heading: qsTrId("gamehq.help.feature.overlay.title"),
                            //% "Transparent fullscreen HUD. View and manage captures from inside a game without alt-tabbing. Includes its own gallery grid, lightbox, and toast notifications."
                            desc: qsTrId("gamehq.help.feature.overlay.description")
                        },
                        {
                            //% "Lightbox"
                            heading: qsTrId("gamehq.help.feature.lightbox.title"),
                            //% "Full-screen viewer for screenshots and videos. Opens from both the main window and the overlay."
                            desc: qsTrId("gamehq.help.feature.lightbox.description")
                        },
                        {
                            //% "Watched folders"
                            heading: qsTrId("gamehq.help.feature.watched_folders.title"),
                            //% "Add any folder (Game Bar, Steam, NVIDIA ShadowPlay) — %1 scans it for new captures automatically."
                            desc: qsTrId("gamehq.help.feature.watched_folders.description").arg(Brand.name)
                        }
                    ]
                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.s4
                        Text {
                            text: modelData.heading
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontH3
                            font.weight: Font.DemiBold
                        }
                        Text {
                            text: modelData.desc
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                        Item { Layout.preferredHeight: Theme.s4 } // spacer between entries
                    }
                }
            }
        }
    }

        }
    }
}
