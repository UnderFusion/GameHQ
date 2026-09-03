pragma Singleton
import QtQuick

// Canonical product branding. Change these values for a future visible rename;
// CMake also generates the matching C++ constants from this file.
QtObject {
    readonly property string name: "GameHQ"
    readonly property string slug: "gamehq"

    // Canonical project links. Website points at the GitHub repository until
    // a real public site is deployed (see START-PLAN Phase 0 decisions).
    readonly property string repositoryUrl: "https://github.com/underfusion/GameHQ"
    readonly property string websiteUrl: repositoryUrl
    readonly property string releasesUrl: repositoryUrl + "/releases"
    readonly property string issuesUrl: repositoryUrl + "/issues"
    readonly property string licenseUrl: repositoryUrl + "/blob/main/LICENSE"
    readonly property string securityUrl: repositoryUrl + "/blob/main/docs/security-and-privacy.md"
    readonly property string supportUrl: "https://ko-fi.com/underfusion"
}
