import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import GameHQ

Item {
    id: root
    Layout.fillWidth: true
    Layout.fillHeight: true

    // The grid is told what to show and asks for what it wants done; it does
    // not reach into the window it happens to be parented to. Everything the
    // host used to expose arrives as a property, every host call leaves as a
    // signal. Signal handlers run synchronously, so the emit order below IS
    // the old call order — that matters on the key path, where the
    // usingGamepad write has to land before input.handleKeyPressed() runs.
    property int columns: 1
    property real zoomLevel: 0
    property bool bulkMode: false
    // Predicate, not a window: (filePath) -> bool. Kept callable rather than
    // turned into a signal so the delegate's `checked` stays a real binding
    // that re-evaluates when the selection changes.
    property var bulkIsChecked: (filePath) => false
    property var deleteDialog
    property var bulkDeleteDialog
    property alias currentIndex: galleryGrid.currentIndex
    property alias count: galleryGrid.count
    property alias cellWidth: galleryGrid.cellWidth
    // The view is inset from this Item, so the host cannot derive the column
    // count from root.width — it needs the width the cells actually divide.
    property alias viewWidth: galleryGrid.width
    property alias _navLockUntil: galleryGrid._navLockUntil

    signal captureActivated(int index)
    signal deleteRequested(int index, string gameName, string dateText)
    signal addFolderRequested()
    signal keyboardActivity()
    signal bulkToggleRequested(int index, bool extendRange)
    signal bulkDeleteRequested()
    signal bulkSelectAllRequested()

    function forceActiveFocus() { galleryGrid.forceActiveFocus() }
    function moveCurrentIndexLeft() { galleryGrid.moveCurrentIndexLeft() }
    function moveCurrentIndexRight() { galleryGrid.moveCurrentIndexRight() }
    function moveCurrentIndexUp() { galleryGrid.moveCurrentIndexUp() }
    function moveCurrentIndexDown() { galleryGrid.moveCurrentIndexDown() }

    // Called after every pad/keyboard selection move: guarantees the selected
    // tile is fully on screen even when the view was wheel-scrolled away from
    // the selection beforehand.
    function revealCurrent() {
        if (galleryGrid.currentIndex >= 0)
            galleryGrid.positionViewAtIndex(galleryGrid.currentIndex, GridView.Contain)
    }

    // Wheel-like pad scroll (right stick): moves the viewport, not the
    // selection. Same step feel as HelpView.scrollBy.
    function scrollBy(direction) {
        const maximum = Math.max(0, galleryGrid.contentHeight - galleryGrid.height)
        galleryGrid.contentY = Math.max(0, Math.min(maximum,
            galleryGrid.contentY + direction * Math.max(80, galleryGrid.height * 0.28)))
    }

    GridView {
        id: galleryGrid
        anchors.fill: parent
        anchors.leftMargin: -Theme.s8
        anchors.rightMargin: Theme.s12
        clip: true
        model: app.gallery

        property point _lastHoverPos: Qt.point(-9999, -9999)
        property real _navLockUntil: 0

        HoverHandler { id: gridHover }

        cellWidth: galleryGrid.width > 0
            ? galleryGrid.width / root.columns
            : root.zoomLevel
        cellHeight: galleryGrid.cellWidth * 9 / 16 + Theme.s32
        focus: true
        keyNavigationEnabled: false
        onCurrentIndexChanged: sounds.play("nav_tick")

        delegate: Item {
            width: galleryGrid.cellWidth
            height: galleryGrid.cellHeight

            HoverHandler {
                onHoveredChanged: {
                    if (!hovered) return
                    if (Date.now() < galleryGrid._navLockUntil) return
                    const p = gridHover.point.position
                    const px = Math.round(p.x), py = Math.round(p.y)
                    if (px !== galleryGrid._lastHoverPos.x || py !== galleryGrid._lastHoverPos.y) {
                        galleryGrid._lastHoverPos = Qt.point(px, py)
                        galleryGrid.currentIndex = index
                    }
                }
            }

            CaptureTile {
                anchors.fill: parent
                anchors.margins: Theme.s8
                thumbnail: model.thumbnail
                captureType: model.captureType
                gameName: model.gameName
                dateText: model.dateText
                favorite: model.favorite
                selected: galleryGrid.currentIndex === index
                bulkMode: root.bulkMode
                checked: root.bulkIsChecked(model.filePath)
                onActivated: {
                    galleryGrid.currentIndex = index
                    galleryGrid.forceActiveFocus()
                    root.captureActivated(index)
                }
                onCheckToggled: function(extendRange) {
                    galleryGrid.currentIndex = index
                    galleryGrid.forceActiveFocus()
                    root.bulkToggleRequested(index, extendRange)
                }
                onOpenRequested: {
                    sounds.play("confirm")
                    app.openCapture(index)
                }
                onRequestOpenFolder: app.showInFolder(index)
                onToggleFavoriteRequested: {
                    sounds.play("favorite")
                    app.toggleFavorite(index)
                }
                onRequestDelete: root.deleteRequested(index, model.gameName, model.dateText)
            }
        }

        Keys.onPressed: (event) => {
            root.keyboardActivity()
            if (input.handleKeyPressed(event.key, event.modifiers,
                                       event.isAutoRepeat)) {
                event.accepted = true
                return
            }

            if (root.bulkMode && !root.bulkDeleteDialog.visible) {
                if (event.key === Qt.Key_Space) {
                    const rec = app.gallery.get(currentIndex)
                    if (rec.filePath)
                        root.bulkToggleRequested(currentIndex,
                                                 !!(event.modifiers & Qt.ShiftModifier))
                    event.accepted = true
                    return
                } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
                    root.bulkDeleteRequested()
                    event.accepted = true
                    return
                } else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                    root.bulkSelectAllRequested()
                    event.accepted = true
                    return
                }
            }

            if (!root.bulkMode && event.key === Qt.Key_E) {
                app.showInFolder(currentIndex)
                event.accepted = true
            }
        }

        Keys.onReleased: (event) => {
            event.accepted = input.handleKeyReleased(event.key, event.modifiers)
        }

        DesktopEmptyState {
            visible: galleryGrid.count === 0
            onAddFolderRequested: root.addFolderRequested()
        }
    }

    ScrollBar {
        id: gridScroll
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        orientation: Qt.Vertical
        policy: ScrollBar.AlwaysOn
        size: galleryGrid.visibleArea.heightRatio
        Binding on position {
            value: galleryGrid.visibleArea.yPosition
            when: !gridScroll.pressed
            restoreMode: Binding.RestoreBinding
        }
        onPositionChanged: {
            if (gridScroll.pressed)
                // ScrollBar.position is a fraction of the whole content,
                // with its maximum at 1 - size, not at 1.
                galleryGrid.contentY = galleryGrid.originY
                    + gridScroll.position * galleryGrid.contentHeight
        }
        interactive: true
        contentItem: Rectangle {
            implicitWidth: 6
            implicitHeight: 64
            radius: 3
            color: gridScroll.hovered || gridScroll.pressed
                ? Theme.accent : Theme.textFaint
            opacity: gridScroll.pressed ? 1.0 : gridScroll.hovered ? 0.85 : 0.5
            Behavior on color {
                ColorAnimation { duration: Theme.durFast }
            }
            Behavior on opacity {
                NumberAnimation { duration: Theme.durFast }
            }
        }
        background: Rectangle {
            implicitWidth: 6
            color: "transparent"
            radius: 3
        }
        padding: 0
    }
}
