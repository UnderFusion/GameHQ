.pragma library

// Groups of UpdateManager::stateName values shared by every surface that
// reflects the updater (sidebar badge, update banner, About dialog, Settings >
// About). Keep them here so a new state is classified once, not per screen.

// A newer release is known and the updater is acting on it, or failed doing so.
const releaseStates = ["UpdateAvailable", "Downloading", "ReadyToInstall", "PreparingForUpdate",
                       "Quiescent", "Installing", "Failed"]

// The install handshake is running; nothing the user presses can change it.
const installStates = ["PreparingForUpdate", "Quiescent", "Installing"]

function hasRelease(stateName) {
    return releaseStates.includes(stateName)
}

// hasRelease() minus Failed: an update that is still moving forward.
function inProgress(stateName) {
    return stateName !== "Failed" && hasRelease(stateName)
}

function installing(stateName) {
    return installStates.includes(stateName)
}

// `updates` is the UpdateManager context object; reading its properties here
// still registers them as binding dependencies of the caller.
function releaseVisible(updates) {
    return updates.latestVersion !== "" && hasRelease(updates.stateName)
}
