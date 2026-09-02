import QtQuick
import QtQuick.Controls.Basic as QC
import QtQuick.Layouts
import GameHQ

FocusScope {
    id: root

    property bool postUpdateGreeting: false
    property bool showFullNotes: false
    property bool updateReleaseSelected: false
    property int padIndex: 0
    property int bundledReleaseIndex: 0
    signal closed(bool wasPostUpdateGreeting)
    signal updateSettingsRequested()
    signal updateDeferred()

    visible: false
    opacity: 0
    focus: visible

    function hasUpdateRelease() {
        return updates.latestVersion !== ""
            && ["UpdateAvailable", "Downloading", "ReadyToInstall", "PreparingForUpdate",
                "Quiescent", "Installing", "Failed"].includes(updates.stateName)
    }

    function updateVersion() {
        return updates.latestVersion
    }

    function releaseEntries() {
        const entries = []
        if (hasUpdateRelease())
            entries.push({ version: updateVersion(), suffix: "(Available)",
                           isUpdate: true, bundledIndex: -1 })
        const releases = bundledReleases()
        for (let index = 0; index < releases.length; ++index) {
            entries.push({ version: releases[index].version,
                           suffix: index === 0 ? "(Current)" : "",
                           isUpdate: false, bundledIndex: index })
        }
        return entries
    }

    function selectedUsesRemoteNotes() {
        return updateReleaseSelected && hasUpdateRelease()
    }

    function selectedStructuredSections() {
        return selectedBundledRelease().sections || []
    }

    function displayedVersion() {
        if (hasUpdateRelease() && updateReleaseSelected)
            return updateVersion()
        if (!showFullNotes)
            return app.version
        return selectedBundledRelease().version || app.version
    }

    function bundledReleases() {
        return app.releaseNotesReleases || []
    }

    function selectedBundledRelease() {
        const releases = bundledReleases()
        if (releases.length === 0)
            return { version: app.version, date: "", sections: app.releaseNotesSections || [] }
        const index = Math.max(0, Math.min(releases.length - 1, bundledReleaseIndex))
        return releases[index]
    }

    function selectBundledRelease(index) {
        const releases = bundledReleases()
        if (index < 0 || index >= releases.length)
            return
        updateReleaseSelected = false
        bundledReleaseIndex = index
        fullNotesFlick.contentY = 0
        Qt.callLater(focusPadIndex)
    }

    function selectReleaseEntry(entry) {
        if (entry.isUpdate) {
            updateReleaseSelected = true
            fullNotesFlick.contentY = 0
            Qt.callLater(focusPadIndex)
            return
        }
        selectBundledRelease(entry.bundledIndex)
    }

    function releaseVersionControls() {
        const controls = []
        for (const child of releaseVersionLinks.children) {
            if (child.releaseVersionControl === true)
                controls.push(child)
        }
        return controls
    }

    function releaseBadgeText() {
        if (hasUpdateRelease() && updateReleaseSelected)
            return "Available"
        return bundledReleaseIndex === 0 ? "Current" : "Previous"
    }

    function releaseBadgeColor() {
        if (hasUpdateRelease() && updateReleaseSelected)
            return Theme.warning
        return bundledReleaseIndex === 0 ? statusColor() : Theme.textMuted
    }

    function selectedReleaseFooter() {
        if (updateReleaseSelected && hasUpdateRelease()
                && updates.publishedAt.getTime() > 0)
            return "Published " + Qt.formatDate(updates.publishedAt, "d MMM yyyy")
        const release = selectedBundledRelease()
        return release.date ? "Released " + release.date : "Bundled with GameHQ " + app.version
    }

    function statusText() {
        switch (updates.stateName) {
        case "Checking": return "Checking for updates"
        case "UpdateAvailable": return "Update available"
        case "Downloading": return "Downloading " + updates.progress + "%"
        case "ReadyToInstall": return "Ready to install"
        case "PreparingForUpdate":
        case "Quiescent": return "Preparing to install"
        case "Installing": return "Installing"
        case "Failed": return "Update check failed"
        case "UpToDate": return "Up to date"
        default: return updates.lastChecked.getTime() > 0 ? "Up to date" : "Not checked yet"
        }
    }

    function statusColor() {
        if (updates.stateName === "Failed")
            return Theme.danger
        if (["UpdateAvailable", "Downloading", "ReadyToInstall", "PreparingForUpdate",
             "Quiescent", "Installing"].includes(updates.stateName))
            return Theme.warning
        if (updates.stateName === "UpToDate" || updates.lastChecked.getTime() > 0)
            return Theme.success
        return Theme.accent
    }

    function lastCheckedText() {
        if (updates.lastChecked.getTime() <= 0)
            return "Updates have not been checked yet"
        return "Last checked " + Qt.formatDateTime(updates.lastChecked, "d MMM yyyy, HH:mm")
    }

    function primaryLabel() {
        switch (updates.stateName) {
        case "Checking": return "Checking..."
        case "UpdateAvailable": return "Download update " + updates.latestVersion
        case "Downloading": return "Cancel download"
        case "ReadyToInstall": return "Install and restart"
        case "PreparingForUpdate":
        case "Quiescent": return "Preparing..."
        case "Installing": return "Installing..."
        case "Failed": return updates.failedDuringCheck ? "Check again" : "Retry download"
        default: return "Check for updates"
        }
    }

    function primaryEnabled() {
        return !["Checking", "PreparingForUpdate", "Quiescent", "Installing"].includes(updates.stateName)
    }

    function runPrimaryAction() {
        switch (updates.stateName) {
        case "UpdateAvailable": updates.downloadUpdate(); break
        case "Downloading": updates.cancelDownload(); break
        case "ReadyToInstall": updates.installAndRestart(); break
        case "Failed": updates.failedDuringCheck ? updates.checkNow() : updates.downloadUpdate(); break
        case "Checking":
        case "PreparingForUpdate":
        case "Quiescent":
        case "Installing": break
        default: updates.checkNow()
        }
    }

    function summaryItems() {
        const result = []
        if (hasUpdateRelease() && updates.noteBlocks.length > 0) {
            for (const block of updates.noteBlocks) {
                if (block.kind === "heading")
                    continue
                result.push(block.text)
                if (result.length === 3)
                    break
            }
        } else {
            const sections = app.releaseNotesSections || []
            for (const section of sections) {
                const items = section.items || []
                for (const item of items) {
                    result.push(item)
                    if (result.length === 3)
                        return result
                }
            }
        }
        return result
    }

    function escapedStyledText(value) {
        return String(value || "")
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;")
            .replace(/'/g, "&#39;")
    }

    function releaseBlockText(block) {
        if (block.lead === "")
            return escapedStyledText(block.text)
        const separator = block.body === "" || /^\s/.test(block.body) ? "" : " "
        return "<b>" + escapedStyledText(block.lead) + "</b>"
            + separator + escapedStyledText(block.body)
    }

    function focusableControls() {
        if (showFullNotes) {
            const controls = []
            if (!hasUpdateRelease())
                controls.push(backLink)
            if (hasUpdateRelease())
                controls.push(updateNowAction, remindLaterAction, skipUpdateAction)
            if (releaseEntries().length > 1)
                controls.push(...releaseVersionControls())
            return controls.filter(control => control.visible && control.enabled)
        }
        return [releaseNotesLink, primaryAction, settingsButton, skipVersionLink, githubLink,
                issueLink, licenseLink, securityLink, starButton]
            .filter(control => control.visible && control.enabled)
    }

    function focusPadIndex() {
        const controls = focusableControls()
        if (controls.length === 0)
            return
        padIndex = Math.max(0, Math.min(controls.length - 1, padIndex))
        controls[padIndex].forceActiveFocus()
    }

    function padStep(direction) {
        const controls = focusableControls()
        if (controls.length === 0)
            return
        let current = controls.findIndex(control => control.activeFocus)
        if (current < 0)
            current = padIndex
        padIndex = (current + direction + controls.length) % controls.length
        focusPadIndex()
        sounds.play("nav_tick")
    }

    function padVertical(direction) {
        if (!showFullNotes) {
            padStep(direction)
            return
        }
        const maximum = Math.max(0, fullNotesFlick.contentHeight - fullNotesFlick.height)
        fullNotesFlick.contentY = Math.max(0, Math.min(maximum,
            fullNotesFlick.contentY + direction * Math.max(80, fullNotesFlick.height * 0.28)))
        sounds.play("nav_tick")
    }

    function padConfirm() {
        const current = focusableControls().find(control => control.activeFocus)
        if (current)
            current.clicked()
    }

    function openReleaseNotes() {
        showFullNotes = true
        updateReleaseSelected = hasUpdateRelease()
        bundledReleaseIndex = 0
        fullNotesFlick.contentY = 0
        padIndex = 0
        Qt.callLater(focusPadIndex)
    }

    function closeReleaseNotes() {
        if (hasUpdateRelease()) {
            root.close()
            return
        }
        showFullNotes = false
        aboutFlick.contentY = 0
        padIndex = 0
        Qt.callLater(focusPadIndex)
    }

    function open(asPostUpdateGreeting) {
        postUpdateGreeting = !!asPostUpdateGreeting
        showFullNotes = hasUpdateRelease()
        updateReleaseSelected = hasUpdateRelease()
        bundledReleaseIndex = 0
        visible = true
        root.forceActiveFocus()
        aboutFlick.contentY = 0
        fullNotesFlick.contentY = 0
        padIndex = 0
        Qt.callLater(focusPadIndex)
    }

    function close() {
        if (!visible)
            return
        const wasPostUpdate = postUpdateGreeting
        visible = false
        showFullNotes = false
        postUpdateGreeting = false
        updateReleaseSelected = false
        closed(wasPostUpdate)
    }

    Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    states: State {
        when: root.visible
        PropertyChanges { target: root; opacity: 1 }
    }

    HoverHandler {
        blocking: true
    }

    Keys.priority: Keys.BeforeItem
    Keys.onEscapePressed: function(event) {
        event.accepted = true
        if (root.showFullNotes)
            root.closeReleaseNotes()
        else
            root.close()
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            preventStealing: true
            onClicked: root.close()
            onWheel: wheel => wheel.accepted = true
        }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(680, root.width - Theme.s48)
        height: Math.min(720, root.height - Theme.s24)
        radius: Theme.radiusL
        color: Theme.surface
        border.width: Theme.borderWidth
        border.color: Theme.stroke
        clip: true

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            preventStealing: true
            onWheel: wheel => wheel.accepted = true
        }

        DialogCloseButton {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: Theme.s12
            anchors.rightMargin: Theme.s12
            z: 10
            onClicked: root.close()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.s24
            spacing: Theme.s12

            RowLayout {
                visible: !root.showFullNotes
                Layout.fillWidth: true
                Layout.rightMargin: Theme.s48
                spacing: Theme.s16

                Image {
                    source: "qrc:/icons/gamehq.svg"
                    Layout.preferredWidth: Theme.s48
                    Layout.preferredHeight: Theme.s48
                    sourceSize.width: Theme.s48
                    sourceSize.height: Theme.s48
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s4

                    Text {
                        text: root.postUpdateGreeting
                              ? Brand.name + " updated"
                              : Brand.name
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontTitle
                        font.weight: Font.DemiBold
                    }

                    RowLayout {
                        spacing: Theme.s8

                        Text {
                            text: "Version " + app.version
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                        }

                        Rectangle {
                            implicitWidth: modeText.implicitWidth + Theme.s12
                            implicitHeight: 22
                            radius: Theme.radiusPill
                            color: Theme.hoverTint
                            border.width: 1
                            border.color: Theme.borderLight
                            Text {
                                id: modeText
                                anchors.centerIn: parent
                                text: app.portableMode ? "Portable" : "Installed"
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                    }
                }

            }

            RowLayout {
                visible: !root.showFullNotes
                Layout.fillWidth: true
                spacing: Theme.s8

                Rectangle {
                    width: Theme.s12
                    height: Theme.s12
                    radius: Theme.radiusPill
                    color: root.statusColor()
                }
                Text {
                    text: root.statusText()
                    color: root.statusColor()
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    text: "·  " + root.lastCheckedText()
                    color: Theme.textFaint
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                }
            }

            RowLayout {
                visible: root.showFullNotes
                Layout.fillWidth: true
                spacing: Theme.s12

                TextLink {
                    id: backLink
                    visible: !root.hasUpdateRelease()
                    label: "‹  Back"
                    onClicked: root.closeReleaseNotes()
                }
                Text {
                    Layout.fillWidth: true
                    text: root.hasUpdateRelease() ? "New version available" : app.releaseNotesTitle
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontH3
                    font.weight: Font.DemiBold
                }
                Item {
                    Layout.preferredWidth: Theme.s48
                    Layout.preferredHeight: 1
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.stroke
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.showFullNotes ? 1 : 0

                Flickable {
                    id: aboutFlick
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: aboutColumn.implicitHeight + Theme.s8
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick
                    QC.ScrollBar.vertical: AppScrollBar {}

                    ColumnLayout {
                        id: aboutColumn
                        width: Math.max(0, aboutFlick.width - Theme.s16)
                        spacing: Theme.s12

                        Text {
                            Layout.fillWidth: true
                            text: "A controller-friendly screenshot, replay, and media gallery for PC games."
                            textFormat: Text.PlainText
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            text: "WHAT'S NEW IN " + root.displayedVersion()
                            color: Theme.textFaint
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontCaption
                            font.letterSpacing: Theme.letterSpacingWide
                            Layout.topMargin: Theme.s4
                        }

                        Repeater {
                            model: root.summaryItems()
                            delegate: RowLayout {
                                required property string modelData
                                Layout.fillWidth: true
                                spacing: Theme.s8
                                Text {
                                    text: "•"
                                    color: Theme.accent
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontBody
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData
                                    textFormat: Text.PlainText
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontBody
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "Plus more improvements and fixes."
                            textFormat: Text.PlainText
                            color: Theme.textFaint
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontCaption
                        }

                        TextLink {
                            id: releaseNotesLink
                            label: qsTrId("gamehq.about.full_release_notes")
                            suffix: "›"
                            onClicked: root.openReleaseNotes()
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: quickColumn.implicitHeight + Theme.s16 * 2
                            radius: Theme.radiusM
                            color: Theme.surfaceAlt
                            border.width: Theme.borderWidth
                            border.color: Theme.stroke

                            ColumnLayout {
                                id: quickColumn
                                anchors.fill: parent
                                anchors.margins: Theme.s16
                                spacing: Theme.s8

                                Text {
                                    text: "QUICK ACTIONS"
                                    color: Theme.textFaint
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontCaption
                                    font.letterSpacing: Theme.letterSpacingWide
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s8

                                    AccentButton {
                                        id: primaryAction
                                        Layout.fillWidth: true
                                        label: root.primaryLabel()
                                        primary: true
                                        enabled: root.primaryEnabled()
                                        onClicked: root.runPrimaryAction()
                                    }
                                    AccentButton {
                                        id: settingsButton
                                        Layout.fillWidth: true
                                        label: "Update settings"
                                        onClicked: root.updateSettingsRequested()
                                    }
                                }

                                TextLink {
                                    id: skipVersionLink
                                    visible: root.hasUpdateRelease()
                                             && updates.stateName === "UpdateAvailable"
                                    label: "Skip version " + root.updateVersion()
                                    onClicked: {
                                        updates.skipVersion()
                                        root.close()
                                    }
                                }

                                Text {
                                    visible: updates.stateName === "Failed" && updates.errorText !== ""
                                    Layout.fillWidth: true
                                    text: updates.errorText
                                    textFormat: Text.PlainText
                                    color: Theme.danger
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontCaption
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }

                        Text {
                            text: "PROJECT LINKS"
                            color: Theme.textFaint
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontCaption
                            font.letterSpacing: Theme.letterSpacingWide
                            Layout.topMargin: Theme.s4
                        }

                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.s16
                            Layout.preferredHeight: childrenRect.height

                            TextLink {
                                id: githubLink
                                label: "GitHub"
                                onClicked: Qt.openUrlExternally(Brand.repositoryUrl)
                            }
                            TextLink {
                                id: issueLink
                                label: "Report issue"
                                onClicked: Qt.openUrlExternally(Brand.issuesUrl)
                            }
                            TextLink {
                                id: licenseLink
                                label: "License"
                                onClicked: Qt.openUrlExternally(Brand.licenseUrl)
                            }
                            TextLink {
                                id: securityLink
                                label: "Security & privacy"
                                onClicked: Qt.openUrlExternally(Brand.securityUrl)
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: supportRow.implicitHeight + Theme.s12 * 2
                            radius: Theme.radiusM
                            color: Theme.hoverTint

                            RowLayout {
                                id: supportRow
                                anchors.fill: parent
                                anchors.margins: Theme.s12
                                spacing: Theme.s12

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s4
                                    Text {
                                        Layout.fillWidth: true
                                        text: "Enjoying GameHQ?"
                                        color: Theme.text
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontBody
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: "A GitHub star helps more players discover the project."
                                        color: Theme.textFaint
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontCaption
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                AccentButton {
                                    id: starButton
                                    label: "Star on GitHub"
                                    onClicked: Qt.openUrlExternally(Brand.repositoryUrl)
                                }
                            }
                        }
                    }
                }

                Flickable {
                    id: fullNotesFlick
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: fullNotesColumn.implicitHeight + Theme.s8
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick
                    QC.ScrollBar.vertical: AppScrollBar {}

                    ColumnLayout {
                        id: fullNotesColumn
                        width: Math.max(0, fullNotesFlick.width - Theme.s16)
                        spacing: Theme.s16

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: "Version " + root.displayedVersion()
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontTitle
                                font.weight: Font.DemiBold
                            }
                            Rectangle {
                                implicitWidth: latestText.implicitWidth + Theme.s16
                                implicitHeight: 24
                                radius: Theme.radiusPill
                                color: Theme.hoverTint
                                border.width: 1
                                border.color: root.releaseBadgeColor()
                                Text {
                                    id: latestText
                                    anchors.centerIn: parent
                                    text: root.releaseBadgeText()
                                    color: root.releaseBadgeColor()
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontCaption
                                }
                            }
                        }

                        Rectangle {
                            visible: root.hasUpdateRelease()
                            Layout.fillWidth: true
                            implicitHeight: updateActionColumn.implicitHeight + Theme.s16 * 2
                            radius: Theme.radiusM
                            color: Theme.surfaceAlt
                            border.width: Theme.borderWidth
                            border.color: Theme.stroke

                            ColumnLayout {
                                id: updateActionColumn
                                anchors.fill: parent
                                anchors.margins: Theme.s16
                                spacing: Theme.s8

                                Text {
                                    Layout.fillWidth: true
                                    text: "Choose when to install " + Brand.name + " " + root.updateVersion() + "."
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontCaption
                                    wrapMode: Text.WordWrap
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s8

                                    AccentButton {
                                        id: updateNowAction
                                        Layout.fillWidth: true
                                        label: root.primaryLabel()
                                        primary: true
                                        enabled: root.primaryEnabled()
                                        onClicked: root.runPrimaryAction()
                                    }

                                    AccentButton {
                                        id: remindLaterAction
                                        Layout.fillWidth: true
                                        label: "Remind me later"
                                        visible: updates.stateName === "UpdateAvailable"
                                                 || updates.stateName === "Failed"
                                        onClicked: {
                                            root.updateDeferred()
                                            root.close()
                                        }
                                    }

                                    AccentButton {
                                        id: skipUpdateAction
                                        Layout.fillWidth: true
                                        visible: updates.stateName === "UpdateAvailable"
                                        label: "Skip this version"
                                        onClicked: {
                                            updates.skipVersion()
                                            root.close()
                                        }
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            visible: root.releaseEntries().length > 1
                            Layout.fillWidth: true
                            spacing: Theme.s8

                            Text {
                                text: "VERSIONS"
                                color: Theme.textFaint
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontCaption
                                font.letterSpacing: Theme.letterSpacingWide
                            }

                            Flow {
                                id: releaseVersionLinks
                                Layout.fillWidth: true
                                Layout.preferredHeight: childrenRect.height
                                spacing: Theme.s16

                                Repeater {
                                    model: root.releaseEntries()
                                    delegate: TextLink {
                                        required property int index
                                        required property var modelData
                                        property bool releaseVersionControl: true
                                        label: modelData.version
                                        selected: modelData.isUpdate
                                                  ? root.updateReleaseSelected
                                                  : !root.updateReleaseSelected
                                                    && modelData.bundledIndex === root.bundledReleaseIndex
                                        suffix: modelData.suffix
                                        suffixColor: modelData.isUpdate
                                                     ? Theme.success : Theme.textFaint
                                        suffixFontSize: Theme.fontCaption
                                        onClicked: root.selectReleaseEntry(modelData)
                                    }
                                }
                            }
                        }

                        Repeater {
                            model: root.selectedUsesRemoteNotes() ? updates.noteBlocks : []
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 0

                                Text {
                                    visible: modelData.kind === "heading"
                                    Layout.fillWidth: true
                                    text: modelData.text.toUpperCase()
                                    textFormat: Text.PlainText
                                    color: Theme.textFaint
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontCaption
                                    font.letterSpacing: Theme.letterSpacingWide
                                }

                                RowLayout {
                                    visible: modelData.kind === "bullet"
                                    Layout.fillWidth: true
                                    spacing: Theme.s8

                                    Text {
                                        Layout.alignment: Qt.AlignTop
                                        text: "\u2022"
                                        color: Theme.accent
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontBody
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.releaseBlockText(modelData)
                                        textFormat: Text.StyledText
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontBody
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                Text {
                                    visible: modelData.kind === "paragraph"
                                    Layout.fillWidth: true
                                    text: root.releaseBlockText(modelData)
                                    textFormat: Text.StyledText
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontBody
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }

                        Repeater {
                            model: root.selectedUsesRemoteNotes() ? []
                                                                  : root.selectedStructuredSections()
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Theme.s8

                                Text {
                                    text: modelData.title.toUpperCase()
                                    color: Theme.textFaint
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontCaption
                                    font.letterSpacing: Theme.letterSpacingWide
                                }

                                Repeater {
                                    model: modelData.items
                                    delegate: RowLayout {
                                        required property string modelData
                                        Layout.fillWidth: true
                                        spacing: Theme.s8
                                        Text {
                                            text: "•"
                                            color: Theme.accent
                                            font.family: Theme.fontFamily
                                            font.pixelSize: Theme.fontBody
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData
                                            textFormat: Text.PlainText
                                            color: Theme.textMuted
                                            font.family: Theme.fontFamily
                                            font.pixelSize: Theme.fontBody
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: Theme.stroke
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root.selectedReleaseFooter()
                            color: Theme.textFaint
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                }
            }

        }
    }
}
