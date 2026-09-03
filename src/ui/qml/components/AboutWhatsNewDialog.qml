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
            entries.push({ version: updateVersion(), suffix: availableSuffix(),
                           isUpdate: true, bundledIndex: -1 })
        const releases = bundledReleases()
        for (let index = 0; index < releases.length; ++index) {
            entries.push({ version: releases[index].version,
                           suffix: index === 0 ? currentSuffix() : "",
                           isUpdate: false, bundledIndex: index })
        }
        return entries
    }

    function availableSuffix() {
        //% "(Available)"
        return qsTrId("gamehq.update.suffix.available")
    }

    function currentSuffix() {
        //% "(Current)"
        return qsTrId("gamehq.update.suffix.current")
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
        if (hasUpdateRelease() && updateReleaseSelected) {
            //% "Available"
            return qsTrId("gamehq.update.status.available")
        }
        if (bundledReleaseIndex === 0) {
            //% "Current"
            return qsTrId("gamehq.update.status.current")
        }
        //% "Previous"
        return qsTrId("gamehq.update.status.previous")
    }

    function releaseBadgeColor() {
        if (hasUpdateRelease() && updateReleaseSelected)
            return Theme.warning
        return bundledReleaseIndex === 0 ? statusColor() : Theme.textMuted
    }

    function selectedReleaseFooter() {
        if (updateReleaseSelected && hasUpdateRelease()
                && updates.publishedAt.getTime() > 0) {
            //% "Published %1"
            return qsTrId("gamehq.update.published_on")
                    .arg(languageManager.formatDate(updates.publishedAt))
        }
        const release = selectedBundledRelease()
        if (release.date) {
            //% "Released %1"
            return qsTrId("gamehq.update.released_on").arg(release.date)
        }
        //% "Bundled with GameHQ %1"
        return qsTrId("gamehq.update.bundled_with_version").arg(app.version)
    }

    function statusText() {
        switch (updates.stateName) {
        case "Checking":
            //% "Checking for updates"
            return qsTrId("gamehq.update.checking_for_updates_short")
        case "UpdateAvailable":
            //% "Update available"
            return qsTrId("gamehq.update.status.update_available")
        case "Downloading":
            //% "Downloading %1%"
            return qsTrId("gamehq.update.downloading_percent")
                    .arg(languageManager.formatInteger(updates.progress))
        case "ReadyToInstall":
            //% "Ready to install"
            return qsTrId("gamehq.update.ready_to_install")
        case "PreparingForUpdate":
        case "Quiescent":
            //% "Preparing to install"
            return qsTrId("gamehq.update.preparing_to_install")
        case "Installing":
            //% "Installing"
            return qsTrId("gamehq.update.installing")
        case "Failed":
            //% "Update check failed"
            return qsTrId("gamehq.update.check_failed")
        case "UpToDate":
            //% "Up to date"
            return qsTrId("gamehq.update.up_to_date")
        default:
            if (updates.lastChecked.getTime() > 0) {
                //% "Up to date"
                return qsTrId("gamehq.update.up_to_date")
            }
            //% "Not checked yet"
            return qsTrId("gamehq.update.not_checked_yet")
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
        if (updates.lastChecked.getTime() <= 0) {
            //% "Updates have not been checked yet"
            return qsTrId("gamehq.update.not_checked_description")
        }
        //% "Last checked %1"
        return qsTrId("gamehq.update.last_checked")
                .arg(languageManager.formatDateTime(updates.lastChecked))
    }

    function primaryLabel() {
        switch (updates.stateName) {
        case "Checking":
            //% "Checking..."
            return qsTrId("gamehq.update.checking")
        case "UpdateAvailable":
            //% "Download update %1"
            return qsTrId("gamehq.update.download_version").arg(updates.latestVersion)
        case "Downloading":
            //% "Cancel download"
            return qsTrId("gamehq.update.cancel_download")
        case "ReadyToInstall":
            //% "Install and restart"
            return qsTrId("gamehq.update.install_and_restart")
        case "PreparingForUpdate":
        case "Quiescent":
            //% "Preparing..."
            return qsTrId("gamehq.update.preparing")
        case "Installing":
            //% "Installing..."
            return qsTrId("gamehq.update.installing_progress")
        case "Failed":
            if (updates.failedDuringCheck) {
                //% "Check again"
                return qsTrId("gamehq.update.check_again")
            }
            //% "Retry download"
            return qsTrId("gamehq.update.retry_download")
        default:
            //% "Check for updates"
            return qsTrId("gamehq.update.check.label")
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
        if (hasUpdateRelease()) {
            for (const block of updates.noteBlocks) {
                if (block.kind === "heading")
                    continue
                result.push(block.text)
                if (result.length === 3)
                    break
            }
            return result
        }
        const sections = app.releaseNotesSections || []
        for (const section of sections) {
            const items = section.items || []
            for (const item of items) {
                result.push(item)
                if (result.length === 3)
                    return result
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
                        text: {
                            if (root.postUpdateGreeting) {
                                //% "%1 updated"
                                return qsTrId("gamehq.about.product_updated").arg(Brand.name)
                            }
                            return Brand.name
                        }
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontTitle
                        font.weight: Font.DemiBold
                    }

                    RowLayout {
                        spacing: Theme.s8

                        Text {
                            //% "Version %1"
                            text: qsTrId("gamehq.about.version").arg(app.version)
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
                                text: {
                                    if (app.portableMode) {
                                        //% "Portable"
                                        return qsTrId("gamehq.about.mode.portable")
                                    }
                                    //% "Installed"
                                    return qsTrId("gamehq.about.mode.installed")
                                }
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
                    //% "%1  %2"
                    text: qsTrId("gamehq.update.last_checked_with_marker")
                            .arg("·").arg(root.lastCheckedText())
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
                    //% "%1  Back"
                    label: qsTrId("gamehq.action.back_with_marker").arg(
                               languageManager.layoutDirection === Qt.RightToLeft ? "›" : "‹")
                    //% "Back"
                    Accessible.name: qsTrId("gamehq.action.back")
                    onClicked: root.closeReleaseNotes()
                }
                Text {
                    Layout.fillWidth: true
                    text: {
                        if (root.hasUpdateRelease()) {
                            //% "New version available"
                            return qsTrId("gamehq.update.new_version_available")
                        }
                        return app.releaseNotesTitle
                    }
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
                            //% "A controller-friendly screenshot, replay, and media gallery for PC games."
                            text: qsTrId("gamehq.about.product_description")
                            textFormat: Text.PlainText
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            //% "WHAT'S NEW IN %1"
                            text: qsTrId("gamehq.about.whats_new_in").arg(root.displayedVersion())
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
                            //% "Plus more improvements and fixes."
                            text: qsTrId("gamehq.about.more_improvements")
                            textFormat: Text.PlainText
                            color: Theme.textFaint
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontCaption
                        }

                        TextLink {
                            id: releaseNotesLink
                            //% "View full release notes"
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
                                    //% "QUICK ACTIONS"
                                    text: qsTrId("gamehq.about.quick_actions")
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
                                        //% "Update settings"
                                        label: qsTrId("gamehq.update.settings")
                                        onClicked: root.updateSettingsRequested()
                                    }
                                }

                                TextLink {
                                    id: skipVersionLink
                                    visible: root.hasUpdateRelease()
                                             && updates.stateName === "UpdateAvailable"
                                    //% "Skip version %1"
                                    label: qsTrId("gamehq.update.skip_version").arg(root.updateVersion())
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
                            //% "PROJECT LINKS"
                            text: qsTrId("gamehq.about.project_links")
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
                                //% "GitHub"
                                label: qsTrId("gamehq.about.github")
                                onClicked: Qt.openUrlExternally(Brand.repositoryUrl)
                            }
                            TextLink {
                                id: issueLink
                                //% "Report issue"
                                label: qsTrId("gamehq.about.report_issue")
                                onClicked: Qt.openUrlExternally(Brand.issuesUrl)
                            }
                            TextLink {
                                id: licenseLink
                                //% "License"
                                label: qsTrId("gamehq.about.license")
                                onClicked: Qt.openUrlExternally(Brand.licenseUrl)
                            }
                            TextLink {
                                id: securityLink
                                //% "Security & privacy"
                                label: qsTrId("gamehq.settings.about.security_privacy")
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
                                        //% "Enjoying GameHQ?"
                                        text: qsTrId("gamehq.about.enjoying_gamehq")
                                        color: Theme.text
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontBody
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        //% "A GitHub star helps more players discover the project."
                                        text: qsTrId("gamehq.about.star_description")
                                        color: Theme.textFaint
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontCaption
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                AccentButton {
                                    id: starButton
                                    //% "Star on GitHub"
                                    label: qsTrId("gamehq.about.star_on_github")
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
                                //% "Version %1"
                                text: qsTrId("gamehq.about.version").arg(root.displayedVersion())
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
                                    //% "Choose when to install %1 %2."
                                    text: qsTrId("gamehq.update.choose_install_time")
                                            .arg(Brand.name).arg(root.updateVersion())
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
                                        //% "Remind me later"
                                        label: qsTrId("gamehq.update.remind_later")
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
                                        //% "Skip this version"
                                        label: qsTrId("gamehq.update.skip_this_version")
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
                                //% "VERSIONS"
                                text: qsTrId("gamehq.update.versions")
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
