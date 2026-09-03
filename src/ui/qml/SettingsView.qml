import QtQuick
import QtQuick.Layouts
import GameHQ
import "components"
import "helpers/PadNav.js" as PadNav

Item {
    id: root
    signal closeRequested()

    property var categories: [
        //% "General"
        { label: qsTrId("gamehq.settings.category.general"), icon: "\u2699" },
        //% "Capture"
        { label: qsTrId("gamehq.settings.category.capture"), icon: "\u25A3" },
        //% "Replay"
        { label: qsTrId("gamehq.settings.category.replay"), icon: "\u21BA" },
        //% "Input"
        { label: qsTrId("gamehq.settings.category.input"), icon: "\u2328" },
        //% "Library"
        { label: qsTrId("gamehq.settings.category.library"), icon: "\u25A4" },
        //% "Notifications & sound"
        { label: qsTrId("gamehq.settings.category.notifications_sound"), icon: "\u266B" },
        //% "Advanced"
        { label: qsTrId("gamehq.settings.category.advanced"), icon: "\u2261" },
        //% "About"
        { label: qsTrId("gamehq.settings.category.about"), icon: "\u24D8" }
    ]
    property int currentCategory: Math.max(0, Math.min(categories.length - 1,
        Number(app.config("ui.settings_category", 0))))

    function categoryLabel(category) {
        const categoryIndex = ({
            "General": 0,
            "Capture": 1,
            "Replay": 2,
            "Notifications & Sound": 5
        })[category]
        return categoryIndex === undefined ? category : categories[categoryIndex].label
    }

    function selectCategory(index, focusButton) {
        const next = Math.max(0, Math.min(categories.length - 1, index))
        currentCategory = next
        app.setConfig("ui.settings_category", next)
        if (focusButton && categoryRepeater.itemAt(next))
            categoryRepeater.itemAt(next).forceActiveFocus()
    }
    function activate() {
        if (categoryRepeater.itemAt(currentCategory))
            categoryRepeater.itemAt(currentCategory).forceActiveFocus()
    }
    function restoreFocus() {
        // Restore the panel the controller was using without playing a nav
        // sound. If its options page has no usable control, fall back to the
        // selected category so focus is always visible.
        if (activePanel === panelOptions && focusOptions())
            return
        activePanel = panelCategories
        activate()
    }
    // ───────────────── Pad navigation ─────────────────
    // Settings is three panels: the app sidebar (owned by Main.qml), the
    // category list, and the options for the selected category. Left/Right
    // moves between panels, Up/Down moves inside the active one — so Up/Down
    // must never walk out of the panel it is in, which is exactly what a raw
    // nextItemInFocusChain() walk did.
    readonly property int panelCategories: 0
    readonly property int panelOptions: 1
    property int activePanel: panelCategories

    // The combo whose dropdown the pad opened. While this is set, Up/Down
    // drives the list instead of moving focus, and Cross/Circle commit/cancel.
    property var padCombo: null
    readonly property bool padComboOpen: padCombo !== null && padCombo.popup.visible

    function currentPage() {
        return pageStack.children[root.currentCategory] || null
    }
    function isInside(item, ancestor) {
        return PadNav.isInside(item, ancestor)
    }
    // The modal that owns the pad while it is up: a page-declared dialog
    // (SettingsPage.padOverlay) or one of the confirm dialogs owned here.
    // While one is visible every pad event is routed to it, so focus can
    // never wander into the page underneath.
    function activeOverlay() {
        const page = currentPage()
        if (page && page.padOverlay && page.padOverlay.visible)
            return page.padOverlay
        const locals = [portableImportDialog, resetAllDialog,
                        resetInputDialog, restoreCategoryDialog]
        for (let i = 0; i < locals.length; ++i)
            if (locals[i].visible)
                return locals[i]
        return null
    }
    // First focusable control on the page — where Right lands when entering.
    function firstControlIn(page) {
        const controls = PadNav.focusables(page)
        return controls.length > 0 ? controls[0] : null
    }
    function focusOptions() {
        const page = currentPage()
        // Enter at the top of what is on screen. On an unscrolled page that is
        // the first control anyway; after a right-stick scroll it is the row the
        // user is looking at, so entering never yanks the view back to the top.
        const entry = (page && page.viewportEntryTarget) ? page.viewportEntryTarget(1) : null
        const target = entry || firstControlIn(page)
        if (target) {
            target.forceActiveFocus()
            revealOnPage(page)
            return true
        }
        return false   // a page with nothing to focus keeps the caller on categories
    }

    // Direct reveal after every pad-driven focus move, so the options panel
    // scrolls even if the focus-change signal path ever misses one.
    function revealOnPage(page) {
        if (page && page.revealFocusedItem)
            Qt.callLater(page.revealFocusedItem)
    }

    function enterPanel(panel) {
        if (panel === panelOptions) {
            if (!focusOptions())
                return false
            activePanel = panelOptions
        } else {
            activePanel = panelCategories
            activate()
        }
        sounds.play("nav_tick")
        return true
    }

    function padCategoryStep(direction) {
        if (activeOverlay() || padComboOpen)
            return   // L1/R1 must not switch pages under a modal or open dropdown
        selectCategory((currentCategory + direction + categories.length) % categories.length, true)
        sounds.play("nav_tick")
    }

    // Right-stick scrolling deliberately leaves focus behind, so the pad cursor
    // can end up far above or below what is on screen. The next direction then
    // re-enters at the visible edge instead of snapping the view back to the
    // abandoned row. Returns true when it handled the step.
    function enterFromViewport(page, direction) {
        if (!page || !page.focusInViewport || page.focusInViewport())
            return false
        const entry = page.viewportEntryTarget ? page.viewportEntryTarget(direction) : null
        if (!entry)
            return false
        entry.forceActiveFocus()
        revealOnPage(page)
        sounds.play("nav_tick")
        return true
    }

    // Up/Down inside the options panel: spatial step to the nearest control in
    // the row below/above, never leaving the current page. Spatial, not the
    // raw focus chain, so Down jumps to the next row instead of visiting every
    // segment of a segmented control on the way.
    function optionsStep(direction) {
        const page = currentPage()
        const active = Window.window ? Window.window.activeFocusItem : null
        if (!page)
            return
        if (!active || !isInside(active, page)) {
            focusOptions()
            return
        }
        if (enterFromViewport(page, direction))
            return
        const next = PadNav.verticalTarget(page, active, direction)
        if (next) {
            next.forceActiveFocus()
            revealOnPage(page)
            sounds.play("nav_tick")
        }
    }

    // Right/Left within the focused row — segmented-control segments, the
    // Primary/Secondary assignment slots of a binding card, paired buttons.
    // Returns false at the row edge.
    function optionsStepHorizontal(direction) {
        const page = currentPage()
        const active = Window.window ? Window.window.activeFocusItem : null
        if (!page || !active || !isInside(active, page))
            return false
        // Right re-enters the visible area when focus scrolled off screen; Left
        // keeps its edge meaning so it still backs out to the category list.
        if (direction > 0 && enterFromViewport(page, 1))
            return true
        const next = PadNav.horizontalTarget(page, active, direction)
        if (next) {
            next.forceActiveFocus()
            revealOnPage(page)
            sounds.play("nav_tick")
            return true
        }
        return false
    }

    function padFocusStep(direction) {
        const overlay = activeOverlay()
        if (overlay) {
            if (overlay.padStep)
                overlay.padStep(direction)
            return
        }
        if (padComboOpen) {
            padCombo.padStep(direction)
            sounds.play("nav_tick")
            return
        }
        if (activePanel === panelOptions)
            optionsStep(direction)
        else
            padCategoryStep(direction)
    }

    // Left/Right. A visible modal owns both directions; otherwise Right
    // enters the options panel from the categories and then moves within the
    // focused row, Left walks the row and finally backs out one panel.
    // Returns false when there is nothing further left, so Main.qml can hand
    // focus back to the app sidebar.
    function padPanelStep(direction) {
        const overlay = activeOverlay()
        if (overlay) {
            if (overlay.padHorizontal)
                overlay.padHorizontal(direction)
            return true
        }
        if (padComboOpen)
            return true   // the dropdown owns Left/Right too; swallow it
        if (direction > 0) {
            if (activePanel === panelCategories)
                return enterPanel(panelOptions)
            optionsStepHorizontal(1)
            return true
        }
        if (activePanel === panelOptions) {
            if (!optionsStepHorizontal(-1))
                enterPanel(panelCategories)
            return true
        }
        return false   // already leftmost — caller moves to the sidebar
    }

    function padConfirm() {
        const overlay = activeOverlay()
        if (overlay) {
            if (overlay.padConfirm)
                overlay.padConfirm()
            return
        }
        if (padComboOpen) {
            padCombo.padCommitHighlighted()
            padCombo.popup.close()
            padCombo = null
            sounds.play("confirm")
            return
        }
        const active = Window.window ? Window.window.activeFocusItem : null
        if (active && isInside(active, currentPage()))
            activePanel = panelOptions
        if (activePanel === panelCategories) {
            enterPanel(panelOptions)
            return
        }
        if (!active)
            return
        // A combo opens its list; anything else (toggle, button) just fires.
        if (active.popup && active.padStep) {
            padCombo = active
            active.padBeginHighlight()
            active.popup.open()
            sounds.play("nav_tick")
        } else if (active.clicked) {
            // The click may destroy or disable the very control that fired it
            // (Restore defaults reloads every binding card and greys itself
            // out). Remember where it sat and put the pad cursor back on it —
            // or on whatever now sits closest — so navigation never strands.
            const page = currentPage()
            const point = page ? active.mapToItem(page, active.width / 2,
                                                  active.height / 2) : null
            active.clicked()
            Qt.callLater(function() { root.restoreOptionsFocus(active, point) })
        }
    }

    // After a pad-activated control fired: if focus fell out of the page,
    // return it to the control (when it survived usable) or to the nearest
    // focusable at its old position. A modal or dropdown opened by the click
    // owns the pad instead, so those cases are left alone.
    function restoreOptionsFocus(previous, point) {
        if (activeOverlay() || padComboOpen || activePanel !== panelOptions)
            return
        const page = currentPage()
        if (!page)
            return
        const current = Window.window ? Window.window.activeFocusItem : null
        if (current && isInside(current, page))
            return
        let target = null
        try {
            if (previous && previous.visible && previous.enabled
                    && isInside(previous, page))
                target = previous
        } catch (err) {
            // previous was destroyed by its own action — fall through.
        }
        if (!target && point)
            target = PadNav.nearestTarget(page, point.x, point.y)
        if (target) {
            target.forceActiveFocus()
            revealOnPage(page)
        } else {
            focusOptions()
        }
    }

    // Right stick: wheel-like scroll of whichever surface owns the pad — the
    // open modal's viewport when one is up, otherwise the current options
    // page. An open dropdown swallows it so the page can't shift underneath.
    function padScroll(direction) {
        const overlay = activeOverlay()
        if (overlay) {
            if (overlay.scrollBy)
                overlay.scrollBy(direction)
            return
        }
        if (padComboOpen)
            return
        const page = currentPage()
        if (page && page.scrollBy)
            page.scrollBy(direction)
    }

    // Circle: a modal closes first (the only pad way out of one), then the
    // dropdown closes without committing, then a panel step back. Returns
    // false when there is nothing left to back out of, so Main.qml closes
    // Settings.
    function padBack() {
        const overlay = activeOverlay()
        if (overlay) {
            if (overlay.padBack)
                overlay.padBack()
            return true
        }
        if (padComboOpen) {
            padCombo.popup.close()
            padCombo = null
            return true
        }
        if (activePanel === panelOptions) {
            enterPanel(panelCategories)
            return true
        }
        return false
    }

    onVisibleChanged: {
        if (!visible)
            return
        activePanel = panelCategories
        padCombo = null
        Qt.callLater(activate)
    }

    // A section-wide focus wash is a controller affordance. Moving the mouse
    // switches back to the existing per-control hover/focus treatment.
    // (Qualified through root: a bare Window.window would attach to the
    // HoverHandler, which is not an Item, and always be null.)
    HoverHandler {
        acceptedDevices: PointerDevice.Mouse
        onPointChanged: if (root.Window.window) root.Window.window.usingGamepad = false
    }

    Keys.onPressed: function(event) {
        if (Window.window)
            Window.window.usingGamepad = false
        event.accepted = false
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.s16

        Rectangle {
            Layout.preferredWidth: Theme.s48 * 4
            Layout.fillHeight: true
            radius: Theme.radiusL
            color: Theme.bg1
            border.width: 1
            border.color: Theme.stroke

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.s8
                spacing: Theme.s4
                Repeater {
                    id: categoryRepeater
                    model: root.categories
                    delegate: SettingsCategoryButton {
                        Layout.fillWidth: true
                        label: modelData.label
                        glyph: modelData.icon
                        selected: index === root.currentCategory
                        onClicked: {
                            root.activePanel = root.panelCategories
                            root.selectCategory(index, true)
                        }
                        Keys.onUpPressed: {
                            if (Window.window) Window.window.usingGamepad = false
                            root.selectCategory((index - 1 + root.categories.length) % root.categories.length, true)
                        }
                        Keys.onDownPressed: {
                            if (Window.window) Window.window.usingGamepad = false
                            root.selectCategory((index + 1) % root.categories.length, true)
                        }
                        Keys.onRightPressed: {
                            if (Window.window) Window.window.usingGamepad = false
                            root.enterPanel(root.panelOptions)
                        }
                        Keys.onEscapePressed: root.closeRequested()
                    }
                }
                Item { Layout.fillHeight: true }
            }
        }

        StackLayout {
            id: pageStack
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.fillHeight: true
            currentIndex: root.currentCategory
            GeneralSettingsPage {}
            CaptureSettingsPage {}
            ReplaySettingsPage {}
            InputSettingsPage {}
            LibrarySettingsPage {}
            FeedbackSettingsPage {}
            AdvancedSettingsPage {
                onRestoreAllRequested: resetAllDialog.open()
                onRestoreInputRequested: resetInputDialog.open()
                onRestoreCategoryRequested: function(category) {
                    restoreCategoryDialog.category = category
                    restoreCategoryDialog.open()
                }
                onImportPortableRequested: function(folderUrl) {
                    portableImportDialog.folderUrl = folderUrl
                    portableImportDialog.open()
                }
            }
            AboutSettingsPage {}
        }
    }

    ConfirmDialog {
        id: portableImportDialog
        property url folderUrl
        anchors.fill: parent
        z: 100
        //% "Import this portable profile?"
        title: qsTrId("gamehq.settings.portable_import.title")
        //% "Import is available only into a clean installed profile. Portable captures stay where they are, the source is never modified, and GameHQ restarts to complete the import."
        message: qsTrId("gamehq.settings.portable_import.message")
        //% "Import and restart"
        confirmLabel: qsTrId("gamehq.settings.portable_import.confirm")
        onConfirmed: {
            const error = app.beginPortableImport(folderUrl)
            if (error !== "") {
                //% "Portable import failed"
                notifications.post(qsTrId("gamehq.settings.portable_import.failed"), error, "", "error")
                sounds.play("error")
            }
        }
    }

    ConfirmDialog {
        id: resetAllDialog
        anchors.fill: parent
        z: 100
        //% "Restore all settings?"
        title: qsTrId("gamehq.settings.restore_all.title")
        //% "Window preferences, capture behavior, replay options, notifications, and sound settings return to defaults. Captures and library data are not deleted."
        message: qsTrId("gamehq.settings.restore_all.message")
        //% "Restore defaults"
        confirmLabel: qsTrId("gamehq.action.restore_defaults")
        onConfirmed: {
            app.resetAllConfig()
            root.currentCategory = 0
            sounds.play("confirm")
        }
    }

    ConfirmDialog {
        id: resetInputDialog
        anchors.fill: parent
        z: 100
        //% "Restore all input bindings?"
        title: qsTrId("gamehq.settings.input.restore_all.title")
        //% "All controller, keyboard, and mouse overrides return to their built-in defaults."
        message: qsTrId("gamehq.settings.input.restore_all.message")
        //% "Restore defaults"
        confirmLabel: qsTrId("gamehq.action.restore_defaults")
        onConfirmed: { input.bindingEditor.resetAllBindings(); sounds.play("confirm") }
    }

    ConfirmDialog {
        id: restoreCategoryDialog
        property string category: ""
        anchors.fill: parent
        z: 100
        //% "Restore %1 settings?"
        title: qsTrId("gamehq.settings.restore_category.title").arg(root.categoryLabel(category))
        //% "%1 options return to defaults. Captures and library data are not deleted."
        message: qsTrId("gamehq.settings.restore_category.message").arg(root.categoryLabel(category))
        //% "Restore defaults"
        confirmLabel: qsTrId("gamehq.action.restore_defaults")
        onConfirmed: {
            app.resetCategory(restoreCategoryDialog.category)
            sounds.play("confirm")
        }
    }
}
