// GameHQ package launcher — the only exe at the clean root (docs/packaging.md).
// Starts app\GameHQ.exe (where all Qt/ffmpeg DLLs live) so the root folder stays
// clean: launcher + README + data folders. Pure Win32, statically linked, no Qt.
#include <windows.h>

#include <string>
#include <vector>

#include "launcher/LauncherCommandLine.h"
#include "launcher/LauncherLocalization.h"
#include "launcher/LauncherResources.h"
#include "launcher/UpdaterPromotion.h"
#include "core/UpdateMaintenance.h"

namespace
{
// GetModuleFileNameW silently truncates into a too-small buffer instead of
// failing, so a package installed below a long path used to look like a
// different, shorter path. Grow until the whole name fits.
bool modulePath(std::wstring& out)
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return false;
        if (length < buffer.size()) {
            out.assign(buffer.data(), length);
            return true;
        }
        if (buffer.size() >= launcher::kMaxCommandLineChars)
            return false;
        buffer.resize(buffer.size() * 2);
    }
}

void showMessage(HINSTANCE instance, LANGID language, unsigned int messageId, UINT icon)
{
    const std::wstring message = launcher::localization::loadString(instance, messageId, language);
    const std::wstring title = launcher::localization::loadString(
        instance, IDS_LAUNCHER_TITLE, language);
    MessageBoxW(nullptr, message.c_str(), title.c_str(), icon);
}
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    std::wstring launcherPath;
    if (!modulePath(launcherPath))
        return 1;
    const std::size_t slash = launcherPath.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return 1;
    const std::wstring root = launcherPath.substr(0, slash);
    const LANGID language = launcher::localization::resolveLanguage(
        std::filesystem::path(root));

    // CreateProcessW resolves its application name and working directory
    // against the classic path limit, so a root that long cannot be started at
    // all. Say so instead of failing with a generic error; automatic updating
    // refuses well before this (UpdatePreflight caps the root at 180).
    if (root.size() + wcslen(L"\\app\\GameHQ.exe") >= MAX_PATH) {
        showMessage(instance, language, IDS_LAUNCHER_PATH_TOO_DEEP, MB_ICONERROR);
        return 1;
    }

    const std::wstring commandLine = GetCommandLineW();
    // A whole-argument match: a capture path that merely contains the text
    // "--post-update" must not be mistaken for the switch.
    const bool postUpdateLaunch = launcher::hasSwitch(commandLine, L"--post-update");
    const maintenance::Info maintenanceState = maintenance::inspect(
        std::filesystem::path(root));
    if (!postUpdateLaunch && maintenanceState.state == maintenance::State::Active) {
        showMessage(instance, language, IDS_LAUNCHER_UPDATE_ACTIVE, MB_ICONINFORMATION);
        return 0;
    }

    // A running helper stages its replacement under a non-running name. The
    // first later launcher validates and promotes it only after the helper's
    // global activity mutex has disappeared.
    launcher::promotePendingUpdater(std::filesystem::path(root));

    const std::wstring exe = root + L"\\app\\GameHQ.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        showMessage(instance, language, IDS_LAUNCHER_EXE_MISSING, MB_ICONERROR);
        return 1;
    }

    // Pass the original arguments through untouched — quoting, embedded spaces
    // and trailing backslashes included — and quote only the child exe path.
    const std::wstring childCommandLine = launcher::buildChildCommandLine(
        exe, launcher::argumentTail(commandLine));
    if (childCommandLine.empty()) {
        showMessage(instance, language, IDS_LAUNCHER_COMMAND_TOO_LONG, MB_ICONERROR);
        return 1;
    }
    std::vector<wchar_t> mutableCommandLine(childCommandLine.begin(), childCommandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Working dir = package root so portable.flag/data resolution stays obvious.
    if (!CreateProcessW(exe.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, root.c_str(), &si, &pi)) {
        showMessage(instance, language, IDS_LAUNCHER_START_FAILED, MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
