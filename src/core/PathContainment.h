#pragma once

// One "is this path inside that root" rule for every updater boundary: the
// app's handoff checks, the post-update token argument and the static helper.
// Header-only so the Qt-free GameHQUpdater and the tests share it unchanged.
//
// Both paths must already be fully qualified. They are then resolved with Win32's own
// rules (GetFullPathNameW: "." and "..", mixed separators, trailing dots and
// spaces) so the comparison sees what Windows would actually open, and are
// compared component by component, case-insensitively. A different drive, a
// UNC share, a device path or a sibling that merely shares a name prefix
// ("C:\GameHQ2" vs "C:\GameHQ") is never inside. Lexical only: callers that
// must also defeat links canonicalize first.

#include <filesystem>
#include <string>
#include <windows.h>

namespace path_containment
{
namespace detail
{
inline bool isSeparator(wchar_t ch) { return ch == L'\\' || ch == L'/'; }

// "X:\..." or a UNC / device path "\\...". libstdc++'s path::is_absolute does
// not recognise UNC roots on MinGW, and "C:foo" or "\foo" depend on process
// state, so neither is accepted.
inline bool isFullyQualified(const std::wstring &value)
{
    if (value.size() >= 3 && ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z'))
        && value[1] == L':' && isSeparator(value[2]))
        return true;
    return value.size() >= 3 && isSeparator(value[0]) && isSeparator(value[1])
        && !isSeparator(value[2]);
}

inline bool win32FullPath(const std::filesystem::path &path, std::filesystem::path &out)
{
    const std::wstring input = path.native();
    if (!isFullyQualified(input))
        return false;
    DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0)
        return false;
    std::wstring full(needed, L'\0');
    const DWORD written = GetFullPathNameW(input.c_str(), needed, full.data(), nullptr);
    if (written == 0 || written >= needed)
        return false;
    full.resize(written);
    // "C:\root\" and "C:\root" are the same directory; keep a bare "C:\".
    while (full.size() > 3 && isSeparator(full.back()))
        full.pop_back();
    out = std::filesystem::path(full);
    return isFullyQualified(full);
}

inline bool sameComponent(const std::filesystem::path &a, const std::filesystem::path &b)
{
    const std::wstring &left = a.native();
    const std::wstring &right = b.native();
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
} // namespace detail

inline bool isWithinRoot(const std::filesystem::path &root,
                         const std::filesystem::path &candidate, bool allowRoot = false)
{
    std::filesystem::path fullRoot;
    std::filesystem::path fullCandidate;
    if (!detail::win32FullPath(root, fullRoot) || !detail::win32FullPath(candidate, fullCandidate))
        return false;
    auto rootIt = fullRoot.begin();
    auto candidateIt = fullCandidate.begin();
    for (; rootIt != fullRoot.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == fullCandidate.end() || !detail::sameComponent(*rootIt, *candidateIt))
            return false;
    }
    // "\\?\" paths skip Win32 normalisation, so a surviving "." or ".." is
    // never trusted to stay below the root.
    for (auto rest = candidateIt; rest != fullCandidate.end(); ++rest) {
        if (rest->native() == L"." || rest->native() == L"..")
            return false;
    }
    return allowRoot || candidateIt != fullCandidate.end();
}
} // namespace path_containment
