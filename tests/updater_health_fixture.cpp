#include <chrono>
#include <string>
#include <thread>
#include <windows.h>

int wmain(int argc, wchar_t **argv)
{
    if (argc == 1) {
        HANDLE marker = CreateFileW(L"previous-started.token", GENERIC_WRITE, 0,
                                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (marker == INVALID_HANDLE_VALUE)
            return 5;
        CloseHandle(marker);
        return 0;
    }
    if (argc != 4 || std::wstring(argv[1]) != L"--post-update")
        return 2;
    const std::wstring version(argv[2]);
    if (version == L"9.9.8")
        return 0;
    if (version == L"9.9.7") {
        // Alive but never healthy: only the helper's deadline ends this.
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    HANDLE file = CreateFileW(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 3;
    std::string bytes(version.begin(), version.end());
    bytes.push_back('\n');
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr);
    CloseHandle(file);
    return ok && written == bytes.size() ? 0 : 4;
}
