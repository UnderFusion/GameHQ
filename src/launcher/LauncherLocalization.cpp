#include "launcher/LauncherLocalization.h"

#include <array>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace
{
using launcher::localization::kEnglishLanguage;

struct LocaleAlias
{
    const wchar_t* alias;
    const wchar_t* canonical;
    LANGID language;
};

constexpr LANGID lang(WORD primary, WORD sublanguage)
{
    return MAKELANGID(primary, sublanguage);
}

constexpr LocaleAlias kAliases[] = {
    {L"en-us", L"en-US", kEnglishLanguage}, {L"en", L"en-US", kEnglishLanguage},
    {L"zh-hans", L"zh-Hans", lang(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)},
    {L"zh-cn", L"zh-Hans", lang(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)},
    {L"zh-sg", L"zh-Hans", lang(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)},
    {L"zh", L"zh-Hans", lang(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)},
    {L"ru-ru", L"ru-RU", lang(LANG_RUSSIAN, SUBLANG_DEFAULT)},
    {L"ru", L"ru-RU", lang(LANG_RUSSIAN, SUBLANG_DEFAULT)},
    {L"es-es", L"es-ES", lang(LANG_SPANISH, SUBLANG_SPANISH_MODERN)},
    {L"es", L"es-ES", lang(LANG_SPANISH, SUBLANG_SPANISH_MODERN)},
    {L"pt-br", L"pt-BR", lang(LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN)},
    {L"pt", L"pt-BR", lang(LANG_PORTUGUESE, SUBLANG_PORTUGUESE_BRAZILIAN)},
    {L"de-de", L"de-DE", lang(LANG_GERMAN, SUBLANG_GERMAN)},
    {L"de", L"de-DE", lang(LANG_GERMAN, SUBLANG_GERMAN)},
    {L"ja-jp", L"ja-JP", lang(LANG_JAPANESE, SUBLANG_DEFAULT)},
    {L"ja", L"ja-JP", lang(LANG_JAPANESE, SUBLANG_DEFAULT)},
    {L"fr-fr", L"fr-FR", lang(LANG_FRENCH, SUBLANG_FRENCH)},
    {L"fr", L"fr-FR", lang(LANG_FRENCH, SUBLANG_FRENCH)},
    {L"pl-pl", L"pl-PL", lang(LANG_POLISH, SUBLANG_DEFAULT)},
    {L"pl", L"pl-PL", lang(LANG_POLISH, SUBLANG_DEFAULT)},
    {L"ko-kr", L"ko-KR", lang(LANG_KOREAN, SUBLANG_DEFAULT)},
    {L"ko", L"ko-KR", lang(LANG_KOREAN, SUBLANG_DEFAULT)},
    {L"zh-hant", L"zh-Hant", lang(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)},
    {L"zh-tw", L"zh-Hant", lang(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)},
    {L"zh-hk", L"zh-Hant", lang(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)},
    {L"tr-tr", L"tr-TR", lang(LANG_TURKISH, SUBLANG_DEFAULT)},
    {L"tr", L"tr-TR", lang(LANG_TURKISH, SUBLANG_DEFAULT)},
    {L"th-th", L"th-TH", lang(LANG_THAI, SUBLANG_DEFAULT)},
    {L"th", L"th-TH", lang(LANG_THAI, SUBLANG_DEFAULT)},
    {L"es-419", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-mx", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-ar", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-bo", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-cl", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-co", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-cr", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-do", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-ec", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-gt", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-hn", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-ni", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-pa", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-pe", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-pr", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-py", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-sv", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-us", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-uy", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"es-ve", L"es-419", lang(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN)},
    {L"uk-ua", L"uk-UA", lang(LANG_UKRAINIAN, SUBLANG_DEFAULT)},
    {L"uk", L"uk-UA", lang(LANG_UKRAINIAN, SUBLANG_DEFAULT)},
    {L"it-it", L"it-IT", lang(LANG_ITALIAN, SUBLANG_ITALIAN)},
    {L"it", L"it-IT", lang(LANG_ITALIAN, SUBLANG_ITALIAN)},
};

std::wstring normalized(std::wstring_view value)
{
    std::size_t first = 0;
    while (first < value.size() && std::iswspace(value[first]))
        ++first;
    std::size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1]))
        --last;

    std::wstring result(value.substr(first, last - first));
    for (wchar_t& character : result) {
        if (character == L'_')
            character = L'-';
        else
            character = std::towlower(character);
    }
    return result;
}

void skipSpace(const std::string& json, std::size_t& offset)
{
    while (offset < json.size()
           && (json[offset] == ' ' || json[offset] == '\t'
               || json[offset] == '\r' || json[offset] == '\n'))
        ++offset;
}

int hexValue(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

bool parseJsonString(const std::string& json, std::size_t& offset, std::string* value)
{
    if (offset >= json.size() || json[offset] != '"')
        return false;
    ++offset;
    if (value)
        value->clear();

    while (offset < json.size()) {
        const unsigned char character = static_cast<unsigned char>(json[offset++]);
        if (character == '"')
            return true;
        if (character < 0x20)
            return false;
        if (character != '\\') {
            if (value)
                value->push_back(static_cast<char>(character));
            continue;
        }
        if (offset >= json.size())
            return false;
        const char escape = json[offset++];
        switch (escape) {
        case '"': case '\\': case '/':
            if (value) value->push_back(escape);
            break;
        case 'b': if (value) value->push_back('\b'); break;
        case 'f': if (value) value->push_back('\f'); break;
        case 'n': if (value) value->push_back('\n'); break;
        case 'r': if (value) value->push_back('\r'); break;
        case 't': if (value) value->push_back('\t'); break;
        case 'u': {
            if (offset + 4 > json.size())
                return false;
            int codePoint = 0;
            for (int digit = 0; digit < 4; ++digit) {
                const int part = hexValue(json[offset++]);
                if (part < 0)
                    return false;
                codePoint = codePoint * 16 + part;
            }
            if (value) {
                if (codePoint > 0 && codePoint <= 0x7f)
                    value->push_back(static_cast<char>(codePoint));
                else
                    value->push_back('?');
            }
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

bool skipJsonValue(const std::string& json, std::size_t& offset, unsigned depth)
{
    if (depth > 64)
        return false;
    skipSpace(json, offset);
    if (offset >= json.size())
        return false;
    if (json[offset] == '"')
        return parseJsonString(json, offset, nullptr);
    if (json[offset] == '{') {
        ++offset;
        skipSpace(json, offset);
        if (offset < json.size() && json[offset] == '}') {
            ++offset;
            return true;
        }
        for (;;) {
            if (!parseJsonString(json, offset, nullptr))
                return false;
            skipSpace(json, offset);
            if (offset >= json.size() || json[offset++] != ':')
                return false;
            if (!skipJsonValue(json, offset, depth + 1))
                return false;
            skipSpace(json, offset);
            if (offset >= json.size())
                return false;
            if (json[offset] == '}') {
                ++offset;
                return true;
            }
            if (json[offset++] != ',')
                return false;
            skipSpace(json, offset);
        }
    }
    if (json[offset] == '[') {
        ++offset;
        skipSpace(json, offset);
        if (offset < json.size() && json[offset] == ']') {
            ++offset;
            return true;
        }
        for (;;) {
            if (!skipJsonValue(json, offset, depth + 1))
                return false;
            skipSpace(json, offset);
            if (offset >= json.size())
                return false;
            if (json[offset] == ']') {
                ++offset;
                return true;
            }
            if (json[offset++] != ',')
                return false;
        }
    }

    const std::size_t start = offset;
    while (offset < json.size()) {
        const char character = json[offset];
        if (character == ',' || character == '}' || character == ']'
            || character == ' ' || character == '\t'
            || character == '\r' || character == '\n')
            break;
        ++offset;
    }
    const std::string token = json.substr(start, offset - start);
    if (token == "true" || token == "false" || token == "null")
        return true;
    if (token.empty())
        return false;
    char* end = nullptr;
    std::strtod(token.c_str(), &end);
    return end == token.c_str() + token.size();
}

std::optional<std::string> parsePersistedLanguage(const std::string& json)
{
    std::size_t offset = json.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    skipSpace(json, offset);
    if (offset >= json.size() || json[offset++] != '{')
        return std::nullopt;

    std::optional<std::string> language;
    skipSpace(json, offset);
    if (offset < json.size() && json[offset] == '}') {
        ++offset;
    } else {
        for (;;) {
            std::string key;
            if (!parseJsonString(json, offset, &key))
                return std::nullopt;
            skipSpace(json, offset);
            if (offset >= json.size() || json[offset++] != ':')
                return std::nullopt;
            skipSpace(json, offset);

            if (key == "ui.language") {
                if (language.has_value() || !parseJsonString(json, offset, &language.emplace()))
                    return std::nullopt;
            } else if (!skipJsonValue(json, offset, 1)) {
                return std::nullopt;
            }

            skipSpace(json, offset);
            if (offset >= json.size())
                return std::nullopt;
            if (json[offset] == '}') {
                ++offset;
                break;
            }
            if (json[offset++] != ',')
                return std::nullopt;
            skipSpace(json, offset);
        }
    }
    skipSpace(json, offset);
    return offset == json.size() ? language : std::nullopt;
}

std::wstring utf8Ascii(const std::string& value)
{
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if (character > 0x7f)
            return {};
        result.push_back(static_cast<wchar_t>(character));
    }
    return result;
}

std::filesystem::path roamingAppData()
{
    const DWORD required = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (required == 0)
        return {};
    std::vector<wchar_t> buffer(required);
    const DWORD written = GetEnvironmentVariableW(L"APPDATA", buffer.data(), required);
    return written > 0 && written < required
        ? std::filesystem::path(std::wstring(buffer.data(), written))
        : std::filesystem::path();
}

std::vector<std::wstring> preferredWindowsLanguages()
{
    std::vector<std::wstring> languages;
    ULONG count = 0;
    ULONG chars = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &chars)
        && chars > 1) {
        std::vector<wchar_t> buffer(chars);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &chars)) {
            const wchar_t* current = buffer.data();
            while (*current) {
                languages.emplace_back(current);
                current += wcslen(current) + 1;
            }
        }
    }
    if (languages.empty()) {
        std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> locale{};
        if (GetUserDefaultLocaleName(locale.data(), static_cast<int>(locale.size())) > 0)
            languages.emplace_back(locale.data());
    }
    return languages;
}

std::wstring loadForThreadLanguage(HINSTANCE module, unsigned int id, LANGID language)
{
    const LANGID previous = SetThreadUILanguage(language);
    std::array<wchar_t, 2048> buffer{};
    const int length = LoadStringW(module, id, buffer.data(), static_cast<int>(buffer.size()));
    SetThreadUILanguage(previous);
    return length > 0 ? std::wstring(buffer.data(), static_cast<std::size_t>(length))
                      : std::wstring();
}
}

namespace launcher::localization
{
std::wstring canonicalTag(std::wstring_view requested)
{
    const std::wstring key = normalized(requested);
    for (const LocaleAlias& entry : kAliases) {
        if (key == entry.alias)
            return entry.canonical;
    }
    return {};
}

LANGID languageIdForTag(std::wstring_view requested)
{
    const std::wstring key = normalized(requested);
    for (const LocaleAlias& entry : kAliases) {
        if (key == entry.alias)
            return entry.language;
    }
    return kEnglishLanguage;
}

std::filesystem::path configurationPath(const std::filesystem::path& packageRoot,
                                        const std::filesystem::path& roaming)
{
    std::error_code error;
    if (std::filesystem::is_regular_file(packageRoot / L"portable.flag", error))
        return packageRoot / L"gamehq-data" / L"config.json";
    return roaming.empty() ? std::filesystem::path()
                           : roaming / L"GameHQ" / L"config.json";
}

std::wstring readPersistedLanguage(const std::filesystem::path& configPath)
{
    if (configPath.empty())
        return {};
    std::error_code error;
    const auto size = std::filesystem::file_size(configPath, error);
    if (error || size > 1024 * 1024)
        return {};

    std::ifstream input(configPath, std::ios::binary);
    if (!input)
        return {};
    const std::string json((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const std::optional<std::string> language = parsePersistedLanguage(json);
    return language ? utf8Ascii(*language) : std::wstring();
}

LANGID resolveLanguage(std::wstring_view persisted,
                       const std::vector<std::wstring>& windowsUiLanguages)
{
    const std::wstring requested = normalized(persisted);
    if (!requested.empty() && requested != L"system") {
        const std::wstring canonical = canonicalTag(requested);
        if (!canonical.empty())
            return languageIdForTag(canonical);
    }

    for (const std::wstring& uiLanguage : windowsUiLanguages) {
        std::wstring candidate = normalized(uiLanguage);
        while (!candidate.empty()) {
            const std::wstring canonical = canonicalTag(candidate);
            if (!canonical.empty())
                return languageIdForTag(canonical);
            const std::size_t separator = candidate.find_last_of(L'-');
            if (separator == std::wstring::npos)
                break;
            candidate.resize(separator);
        }
    }
    return kEnglishLanguage;
}

LANGID resolveLanguage(const std::filesystem::path& packageRoot)
{
    try {
        const auto config = configurationPath(packageRoot, roamingAppData());
        return resolveLanguage(readPersistedLanguage(config), preferredWindowsLanguages());
    } catch (...) {
        return kEnglishLanguage;
    }
}

std::wstring loadString(HINSTANCE module, unsigned int id, LANGID language)
{
    std::wstring value = loadForThreadLanguage(module, id, language);
    if (value.empty() && language != kEnglishLanguage)
        value = loadForThreadLanguage(module, id, kEnglishLanguage);
    return value;
}
}
