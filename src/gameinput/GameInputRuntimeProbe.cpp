// GameInput runtime probe (Phase 5, t17).
//
// Minimal standalone proof that the Microsoft GameInput runtime can be
// resolved the way the future production backend will resolve it:
//
//   default          — the official MIT loader from the Microsoft.GameInput
//                      package (vendored at third_party/gameinput/) walks
//                      in-box System32 GameInput.dll, the installed
//                      GameInputRedist.dll and an app-local (side-by-side)
//                      GameInputRedist.dll next to this executable, picking
//                      the highest version, and creates the v3 interface.
//   --app-local-only — bypasses the official loader and loads ONLY the
//                      app-local GameInputRedist.dll via an absolute path,
//                      proving Agility-style side-by-side deployment and the
//                      clean fail-soft report when the DLL is absent.
//
// Exit codes: 0 = runtime available, 2 = runtime unavailable (clean degrade
// to the legacy Sony Raw Input/XInput/WinMM stack). The probe must never
// crash and is never required for application startup.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <GameInput.h>

#include "core/DllSearchPolicy.h"

namespace {

std::wstring executableDirectory()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return std::wstring();
        if (length < path.size() - 1) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

void reportLoadedRuntimeModule()
{
    const wchar_t* candidates[] = {L"GameInputRedist.dll", L"GameInput.dll"};
    for (const wchar_t* name : candidates) {
        const HMODULE module = GetModuleHandleW(name);
        if (module == nullptr)
            continue;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(module, path, MAX_PATH) > 0)
            std::printf("runtime-module: %ls\n", path);
    }
}

int reportUnavailable(const char* mode, const HRESULT hr)
{
    std::printf("gameinput-runtime: unavailable (%s, hr=0x%08lX)\n", mode,
                static_cast<unsigned long>(hr));
    std::printf("degrade: legacy backends (Sony Raw Input / XInput / WinMM) remain active\n");
    return 2;
}

int probeOfficialLoader()
{
    std::printf("mode: official-loader\n");
    GameInput::v3::IGameInput* api = nullptr;
    const HRESULT hr = GameInput::v3::GameInputCreate(&api);
    if (FAILED(hr) || api == nullptr)
        return reportUnavailable("official loader found no runtime", hr);
    std::printf("gameinput-runtime: available (v3 interface created)\n");
    reportLoadedRuntimeModule();
    api->Release();
    return 0;
}

int probeAppLocalOnly()
{
    std::printf("mode: app-local-only\n");
    const std::wstring directory = executableDirectory();
    if (directory.empty())
        return reportUnavailable("executable directory unresolved", E_UNEXPECTED);

    const std::wstring dllPath = directory + L"\\GameInputRedist.dll";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("app-local runtime: absent (%ls)\n", dllPath.c_str());
        return reportUnavailable("no side-by-side GameInputRedist.dll",
                                 HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }

    // Same flags as ProductionGameInputApi: dependencies come from the DLL's
    // own folder and System32, never from the CWD or PATH that
    // LOAD_WITH_ALTERED_SEARCH_PATH would still search.
    const HMODULE dll = LoadLibraryExW(dllPath.c_str(), nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                                           | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dll == nullptr)
        return reportUnavailable("app-local LoadLibraryExW failed",
                                 HRESULT_FROM_WIN32(GetLastError()));

    using GameInputInitializeFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto initialize = reinterpret_cast<GameInputInitializeFn>(
        reinterpret_cast<void*>(GetProcAddress(dll, "GameInputInitialize")));
    if (initialize == nullptr) {
        FreeLibrary(dll);
        return reportUnavailable("GameInputInitialize export missing",
                                 HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
    }

    void* raw = nullptr;
    const HRESULT hr = initialize(GameInput::v3::IID_IGameInput, &raw);
    if (FAILED(hr) || raw == nullptr) {
        FreeLibrary(dll);
        return reportUnavailable("app-local runtime rejected v3 interface", hr);
    }

    std::printf("gameinput-runtime: available app-locally (%ls)\n", dllPath.c_str());
    static_cast<GameInput::v3::IGameInput*>(raw)->Release();
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    dll_search::restrictSearchPath();
    const bool appLocalOnly = argc > 1 && std::strcmp(argv[1], "--app-local-only") == 0;
    return appLocalOnly ? probeAppLocalOnly() : probeOfficialLoader();
}
