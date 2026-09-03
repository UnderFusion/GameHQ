import QtQuick
import QtQuick.Layouts
import GameHQ
import "../components"

SettingsPage {
    //% "About"
    pageTitle: qsTrId("gamehq.settings.about.title")
    //% "Version, update status, project resources, and ways to help."
    pageDescription: qsTrId("gamehq.settings.about.description")

    SettingsSection {
        //% "Application"
        eyebrow: qsTrId("gamehq.settings.about.application.eyebrow")
        title: Brand.name
        //% "Version %1"
        status: qsTrId("gamehq.about.version").arg(app.version)
        description: {
            if (app.portableMode) {
                //% "Portable profile"
                return qsTrId("gamehq.settings.about.profile.portable")
            }
            //% "Installed profile"
            return qsTrId("gamehq.settings.about.profile.installed")
        }
        RowLayout {
            Layout.fillWidth: true
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
                        if (updates.stateName === "UpdateAvailable") {
                            //% "GameHQ %1 is available"
                            return qsTrId("gamehq.update.version_available").arg(updates.latestVersion)
                        }
                        //% "GameHQ is ready"
                        return qsTrId("gamehq.settings.about.ready")
                    }
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontH3
                    font.weight: Font.DemiBold
                }
                Text {
                    //% "Free and open source under the GNU GPL version 3."
                    text: qsTrId("gamehq.settings.about.license_summary")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                }
            }
        }
    }

    SettingsSection {
        //% "Maintenance"
        eyebrow: qsTrId("gamehq.settings.about.maintenance.eyebrow")
        //% "Updates"
        title: qsTrId("gamehq.settings.about.updates.title")
        status: {
            if (updates.stateName === "UpdateAvailable") {
                //% "Available"
                return qsTrId("gamehq.update.status.available")
            }
            //% "Current"
            return qsTrId("gamehq.update.status.current")
        }
        description: {
            if (updates.stateName === "Checking") {
                //% "Checking for updates..."
                return qsTrId("gamehq.update.checking_for_updates")
            }
            if (updates.stateName === "UpdateAvailable") {
                //% "GameHQ %1 is available."
                return qsTrId("gamehq.update.version_available_sentence").arg(updates.latestVersion)
            }
            if (updates.stateName === "Downloading") {
                //% "Downloading GameHQ %1... %2%"
                return qsTrId("gamehq.update.downloading_version_progress")
                        .arg(updates.latestVersion)
                        .arg(languageManager.formatInteger(updates.progress))
            }
            if (updates.stateName === "ReadyToInstall") {
                //% "GameHQ %1 is downloaded and SHA-256 verified."
                return qsTrId("gamehq.update.download_verified_version").arg(updates.latestVersion)
            }
            if (updates.stateName === "PreparingForUpdate" || updates.stateName === "Quiescent") {
                //% "Getting ready to install GameHQ %1..."
                return qsTrId("gamehq.update.preparing_version").arg(updates.latestVersion)
            }
            if (updates.stateName === "Installing") {
                //% "Installing GameHQ %1..."
                return qsTrId("gamehq.update.installing_version").arg(updates.latestVersion)
            }
            if (updates.stateName === "Failed" && updates.errorText !== "") return updates.errorText
            if (updates.lastChecked.getTime() > 0) {
                //% "Up to date, last checked %1"
                return qsTrId("gamehq.update.up_to_date_last_checked")
                        .arg(languageManager.formatDateTime(updates.lastChecked))
            }
            //% "GameHQ can check GitHub for newer stable releases."
            return qsTrId("gamehq.update.github_check_description")
        }
        SettingsRow {
            //% "Check automatically"
            label: qsTrId("gamehq.update.check_automatically.label")
            //% "At most once every 24 hours, in the background."
            description: qsTrId("gamehq.update.check_automatically.description")
            SettingsToggle { configKey: "updates.check_automatically"; defaultValue: true }
        }
        SettingsRow {
            //% "Check for updates"
            label: qsTrId("gamehq.update.check.label")
            description: {
                if (updates.stateName === "UpdateAvailable") {
                    //% "%1 available"
                    return qsTrId("gamehq.update.available_version_short").arg(updates.latestVersion)
                }
                //% "Installed: %1"
                return qsTrId("gamehq.update.installed_version").arg(app.version)
            }
            AccentButton {
                id: checkButton
                label: {
                    if (updates.stateName === "Checking") {
                        //% "Checking..."
                        return qsTrId("gamehq.update.checking")
                    }
                    //% "Check now"
                    return qsTrId("gamehq.update.check_now")
                }
                primary: !["UpdateAvailable", "Downloading", "ReadyToInstall", "PreparingForUpdate",
                           "Quiescent", "Installing", "Failed"].includes(updates.stateName)
                quiet: !primary
                enabled: updates.stateName !== "Checking" && updates.stateName !== "Downloading"
                onClicked: updates.checkNow()
            }
        }
        SettingsRow {
            visible: ["UpdateAvailable", "Downloading", "ReadyToInstall", "PreparingForUpdate",
                      "Quiescent", "Installing", "Failed"].includes(updates.stateName)
            label: {
                switch (updates.stateName) {
                case "Downloading":
                    //% "Download progress"
                    return qsTrId("gamehq.update.download_progress")
                case "ReadyToInstall":
                    //% "Ready to install"
                    return qsTrId("gamehq.update.ready_to_install")
                case "PreparingForUpdate":
                case "Quiescent":
                case "Installing":
                    //% "Installing"
                    return qsTrId("gamehq.update.installing")
                default:
                    //% "Beta update download"
                    return qsTrId("gamehq.update.beta_download")
                }
            }
            description: {
                switch (updates.stateName) {
                case "Downloading":
                    //% "%1% complete"
                    return qsTrId("gamehq.update.progress_percent")
                            .arg(languageManager.formatInteger(updates.progress))
                case "ReadyToInstall":
                    //% "GameHQ will restart to apply the update."
                    return qsTrId("gamehq.update.restart_to_apply")
                case "PreparingForUpdate":
                case "Quiescent":
                    //% "Waiting for capture work to finish safely..."
                    return qsTrId("gamehq.update.waiting_for_capture")
                case "Installing":
                    //% "GameHQ is applying the update and will restart."
                    return qsTrId("gamehq.update.applying_and_restarting")
                default:
                    //% "SHA-256 detects corruption, but not a compromised GitHub account."
                    return qsTrId("gamehq.update.sha256_limit_warning")
                }
            }
            AccentButton {
                visible: !["PreparingForUpdate", "Quiescent", "Installing"].includes(updates.stateName)
                label: {
                    switch (updates.stateName) {
                    case "Downloading":
                        //% "Cancel"
                        return qsTrId("gamehq.action.cancel")
                    case "ReadyToInstall":
                        //% "Install and restart"
                        return qsTrId("gamehq.update.install_and_restart")
                    case "Failed":
                        if (updates.failedDuringCheck) {
                            //% "Check again"
                            return qsTrId("gamehq.update.check_again")
                        }
                        //% "Retry download"
                        return qsTrId("gamehq.update.retry_download")
                    default:
                        //% "Download"
                        return qsTrId("gamehq.action.download")
                    }
                }
                primary: updates.stateName !== "Downloading"
                quiet: updates.stateName === "Downloading"
                onClicked: {
                    switch (updates.stateName) {
                    case "Downloading": updates.cancelDownload(); break
                    case "ReadyToInstall": updates.installAndRestart(); break
                    case "Failed": updates.failedDuringCheck ? updates.checkNow() : updates.downloadUpdate(); break
                    default: updates.downloadUpdate()
                    }
                }
            }
        }
        SettingsLinkRow {
            visible: updates.latestVersion !== ""
            icon: "\u2197"
            //% "View release notes"
            label: qsTrId("gamehq.update.view_release_notes")
            description: Brand.releasesUrl
            showDivider: false
            onClicked: updates.openReleasePage()
        }
    }

    SettingsSection {
        //% "Open source"
        eyebrow: qsTrId("gamehq.settings.about.open_source.eyebrow")
        //% "Project"
        title: qsTrId("gamehq.settings.about.project.title")
        //% "Open official GameHQ resources in your default browser."
        description: qsTrId("gamehq.settings.about.project.description")
        SettingsLinkRow {
            icon: "\u2302"
            //% "Website"
            label: qsTrId("gamehq.settings.about.website")
            description: Brand.websiteUrl
            onClicked: Qt.openUrlExternally(Brand.websiteUrl)
        }
        SettingsLinkRow {
            icon: "\u2197"
            //% "Source on GitHub"
            label: qsTrId("gamehq.settings.about.github_source")
            description: Brand.repositoryUrl
            onClicked: Qt.openUrlExternally(Brand.repositoryUrl)
        }
        SettingsLinkRow {
            icon: "\u21BB"
            //% "Releases"
            label: qsTrId("gamehq.settings.about.releases")
            description: Brand.releasesUrl
            onClicked: Qt.openUrlExternally(Brand.releasesUrl)
        }
        SettingsLinkRow {
            icon: "!"
            //% "Report an issue"
            label: qsTrId("gamehq.settings.about.report_issue")
            description: Brand.issuesUrl
            onClicked: Qt.openUrlExternally(Brand.issuesUrl)
        }
        SettingsLinkRow {
            icon: "\u26E8"
            //% "Security & privacy"
            label: qsTrId("gamehq.settings.about.security_privacy")
            //% "Verification, local data, network use, and private reporting"
            description: qsTrId("gamehq.settings.about.security_privacy.description")
            onClicked: Qt.openUrlExternally(Brand.securityUrl)
        }
        SettingsLinkRow {
            icon: "\u00A7"
            //% "GNU GPL v3 License"
            label: qsTrId("gamehq.settings.about.license")
            description: Brand.repositoryUrl + "/blob/main/LICENSE"
            showDivider: false
            onClicked: Qt.openUrlExternally(Brand.repositoryUrl + "/blob/main/LICENSE")
        }
    }

    SettingsSection {
        //% "Community"
        eyebrow: qsTrId("gamehq.settings.about.community.eyebrow")
        //% "Support the project"
        title: qsTrId("gamehq.settings.about.support.title")
        variant: "compact"
        //% "Enjoying %1? A GitHub star helps more players discover the project."
        description: qsTrId("gamehq.settings.about.support.description").arg(Brand.name)
        AccentButton {
            //% "Star %1 on GitHub"
            label: qsTrId("gamehq.settings.about.support.star_on_github").arg(Brand.name)
            quiet: true
            onClicked: Qt.openUrlExternally(Brand.repositoryUrl)
        }
    }
}
