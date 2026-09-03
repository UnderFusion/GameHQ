#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace launcher::localization
{
inline constexpr LANGID kEnglishLanguage = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

// Exact canonical-tag/alias lookup. It deliberately excludes pseudo-locales.
std::wstring canonicalTag(std::wstring_view requested);
LANGID languageIdForTag(std::wstring_view requested);

// The launcher reads the existing semantic setting only. Portable packages
// keep it below their root; installed builds use %APPDATA%\GameHQ.
std::filesystem::path configurationPath(const std::filesystem::path& packageRoot,
                                        const std::filesystem::path& roamingAppData);
std::wstring readPersistedLanguage(const std::filesystem::path& configPath);

// A missing, malformed, or unsupported persisted value defers to Windows UI
// preferences. If Windows has no supported match, en-US is deterministic.
LANGID resolveLanguage(std::wstring_view persisted,
                       const std::vector<std::wstring>& windowsUiLanguages);
LANGID resolveLanguage(const std::filesystem::path& packageRoot);

// Reads the selected STRINGTABLE entry with LoadStringW and retries through
// the complete embedded en-US table when the localized entry is unavailable.
std::wstring loadString(HINSTANCE module, unsigned int id, LANGID language);
}
