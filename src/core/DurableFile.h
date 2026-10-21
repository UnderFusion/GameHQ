#pragma once

// Crash-safe replacement of the small update journals and state files shared
// by the app and the static updater helper. The bytes are written to a pending
// file opened write-through, flushed to the device, and only then renamed over
// the target, so power loss leaves either the old file or the complete new one,
// never a truncated journal. Header-only so the Qt-free GameHQUpdater and every
// test that compiles updater sources get it without another source list entry.

#include <filesystem>
#include <string_view>
#include <windows.h>

namespace durable_file
{
enum class WriteResult { Written, CreateFailed, WriteFailed, PublishFailed };

inline WriteResult replace(const std::filesystem::path &target,
                           const std::filesystem::path &pending, std::string_view bytes)
{
    HANDLE file = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return WriteResult::CreateFailed;
    bool wrote = true;
    std::size_t offset = 0;
    while (wrote && offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(remaining < (1u << 20) ? remaining : (1u << 20));
        DWORD written = 0;
        wrote = WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) && written == chunk;
        offset += written;
    }
    wrote = wrote && FlushFileBuffers(file);
    wrote = CloseHandle(file) && wrote;
    if (!wrote) {
        DeleteFileW(pending.c_str());
        return WriteResult::WriteFailed;
    }
    if (!MoveFileExW(pending.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(pending.c_str());
        return WriteResult::PublishFailed;
    }
    return WriteResult::Written;
}
} // namespace durable_file
