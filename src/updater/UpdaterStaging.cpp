#include "updater/UpdaterStaging.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <system_error>
#include <vector>
#include <windows.h>
#include <bcrypt.h>
#include <miniz.h>

namespace
{
constexpr mz_uint kMaximumFiles = 20000;
constexpr mz_uint64 kMaximumFileBytes = 512ULL * 1024 * 1024;
constexpr mz_uint64 kMaximumTotalBytes = 8ULL * 1024 * 1024 * 1024;

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool hasPrefix(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool validateEntryName(std::string name, std::string &normalized, std::string &error)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.front() == '/' || name.front() == '\\'
        || (name.size() >= 2 && name[1] == ':')) {
        error = "archive contains an absolute or empty path";
        return false;
    }
    while (!name.empty() && name.back() == '/')
        name.pop_back();
    std::size_t begin = 0;
    while (begin <= name.size()) {
        const std::size_t end = name.find('/', begin);
        const std::string part = name.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == ".." || part.find(':') != std::string::npos) {
            error = "archive contains a traversal or invalid path component";
            return false;
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    normalized = name;
    const std::string folded = lower(name);
    const std::vector<std::string> forbidden = {
        "portable.flag", "captures", "captures/", "gamehq-data", "gamehq-data/",
        "saveplay-data", "saveplay-data/", "playhq-data", "playhq-data/"
    };
    for (const std::string &path : forbidden) {
        if (folded == path || hasPrefix(folded, path)) {
            error = "archive contains forbidden user-data path: " + name;
            return false;
        }
    }
    const bool allowed = folded == "gamehq.exe" || folded == "readme.txt"
        || folded == "license.txt" || folded == "third_party_notices.md"
        || folded == "gamehqupdater.pending.exe" || folded == "update-package.json"
        || folded == "app" || hasPrefix(folded, "app/")
        || folded == "licenses" || hasPrefix(folded, "licenses/");
    if (!allowed) {
        error = "archive contains an unexpected root path: " + name;
        return false;
    }
    return true;
}

bool pathWithin(const std::filesystem::path &root, const std::filesystem::path &candidate)
{
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || _wcsicmp(rootIt->c_str(), candidateIt->c_str()) != 0)
            return false;
    }
    return candidateIt != candidate.end();
}

bool utf8Path(const std::string &value, std::filesystem::path &path, std::string &error)
{
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        error = "archive entry path is not valid UTF-8";
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), wide.data(), length) != length) {
        error = "archive entry path could not be decoded";
        return false;
    }
    path = std::filesystem::path(wide);
    return true;
}

struct OutputFile { std::ofstream stream; mz_uint64 offset = 0; };
size_t writeExtracted(void *opaque, mz_uint64 offset, const void *data, size_t size)
{
    auto *output = static_cast<OutputFile *>(opaque);
    if (offset != output->offset)
        return 0;
    output->stream.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    if (!output->stream)
        return 0;
    output->offset += size;
    return size;
}

bool readText(const std::filesystem::path &path, std::string &out)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return out.size() <= 64 * 1024;
}

bool verifySha256(const std::filesystem::path &path, const std::string &expected, std::string &error)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, digestBytes = 0, written = 0;
    std::vector<unsigned char> object, digest;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
        || !BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                             reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &written, 0))
        || !BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                             reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes), &written, 0))) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "could not initialize SHA-256 verification";
        return false;
    }
    object.resize(objectBytes);
    digest.resize(digestBytes);
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, object.data(), objectBytes,
                                         nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "could not initialize the package hash";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> buffer(1024 * 1024);
    bool ok = static_cast<bool>(input);
    while (ok && input) {
        input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && !BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0)))
            ok = false;
    }
    if (input.bad() || !ok || !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), digestBytes, 0)))
        ok = false;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    static constexpr char hex[] = "0123456789abcdef";
    std::string actual;
    actual.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        actual.push_back(hex[byte >> 4]);
        actual.push_back(hex[byte & 0xf]);
    }
    if (!ok || actual != expected) {
        error = ok ? "package SHA-256 changed after download verification"
                   : "could not calculate the package SHA-256";
        return false;
    }
    return true;
}

bool validateManifest(const std::filesystem::path &path, const updater::Transaction &tx,
                      std::string &error)
{
    std::string json;
    if (!readText(path, json)) {
        error = "update-package.json is missing or too large";
        return false;
    }
    const std::regex manifestPattern(
        R"re(^\s*\{\s*"schemaVersion"\s*:\s*([0-9]+)\s*,\s*"productId"\s*:\s*"([^"]+)"\s*,\s*"appVersion"\s*:\s*"([^"]+)"\s*,\s*"layoutVersion"\s*:\s*([0-9]+)\s*,\s*"minimumUpdaterVersion"\s*:\s*"([^"]+)"\s*\}\s*$)re");
    std::smatch match;
    if (!std::regex_match(json, match, manifestPattern)) {
        error = "update-package.json is malformed or has an unexpected schema";
        return false;
    }
    const std::string schema = match[1].str(), product = match[2].str();
    const std::string version = match[3].str(), layout = match[4].str();
    const std::string minimumUpdater = match[5].str();
    if (schema != "1" || product != "underfusion.gamehq" || version != tx.expectedVersion
        || layout != "1" || !std::regex_match(minimumUpdater,
                                               std::regex(R"([0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9})"))) {
        error = "update-package.json does not match this update transaction";
        return false;
    }
    auto parts = [](const std::string &value) {
        std::array<unsigned long, 3> result{};
        std::size_t start = 0;
        for (std::size_t i = 0; i < result.size(); ++i) {
            const std::size_t end = value.find('.', start);
            result[i] = std::stoul(value.substr(start, end - start));
            start = end == std::string::npos ? value.size() : end + 1;
        }
        return result;
    };
    if (parts(GAMEHQ_UPDATER_VERSION) < parts(minimumUpdater)) {
        error = "this package requires a newer updater helper; use the manual update download";
        return false;
    }
    return true;
}
} // namespace

namespace updater
{
bool extractAndValidatePackage(const Transaction &tx, std::string &error)
{
    std::error_code ec;
    if (std::filesystem::exists(tx.stagingDir)) {
        error = "staging directory already exists";
        return false;
    }
    if (!std::filesystem::create_directories(tx.stagingDir, ec) || ec) {
        error = "could not create staging directory";
        return false;
    }
    const auto reject = [&](const std::string &reason) {
        std::error_code cleanupError;
        std::filesystem::remove_all(tx.stagingDir, cleanupError);
        error = cleanupError ? reason + "; staging cleanup also failed" : reason;
        return false;
    };

    if (!verifySha256(tx.packagePath, tx.expectedSha256, error))
        return reject(error);

    mz_zip_archive zip{};
    const std::string package = pathToUtf8(tx.packagePath);
    if (!mz_zip_reader_init_file(&zip, package.c_str(), 0))
        return reject("update package is not a readable ZIP archive");
    struct ZipEnder { mz_zip_archive *zip; ~ZipEnder() { mz_zip_reader_end(zip); } } ender{&zip};

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count == 0 || count > kMaximumFiles)
        return reject("archive file count exceeds the safety limit");
    mz_uint64 totalBytes = 0;
    std::set<std::string> seen;
    bool hasLauncher = false, hasApp = false, hasManifest = false;

    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat))
            return reject("could not inspect an archive entry");
        const mz_uint nameSize = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
        if (nameSize <= 1 || nameSize > 4096)
            return reject("archive entry name exceeds the safety limit");
        std::vector<char> nameBuffer(nameSize);
        if (mz_zip_reader_get_filename(&zip, index, nameBuffer.data(), nameSize) != nameSize)
            return reject("could not read an archive entry name");
        std::string name, rawName(nameBuffer.data());
        const bool directoryEntry = stat.m_is_directory
            || (!rawName.empty() && (rawName.back() == '/' || rawName.back() == '\\'));
        if (!validateEntryName(rawName, name, error))
            return reject(error);
        const std::string folded = lower(name);
        if (!seen.insert(folded).second)
            return reject("archive contains duplicate paths");
        if (stat.m_is_encrypted || !stat.m_is_supported || (stat.m_method != 0 && stat.m_method != 8))
            return reject("archive uses encryption or an unsupported compression method");
        const mz_uint32 unixMode = stat.m_external_attr >> 16;
        if ((unixMode & 0170000) == 0120000 || (stat.m_external_attr & FILE_ATTRIBUTE_REPARSE_POINT))
            return reject("archive contains a link or reparse-point entry");
        if (stat.m_uncomp_size > kMaximumFileBytes || totalBytes > kMaximumTotalBytes - stat.m_uncomp_size)
            return reject("archive uncompressed size exceeds the safety limit");
        totalBytes += stat.m_uncomp_size;

        std::filesystem::path relativePath;
        if (!utf8Path(name, relativePath, error))
            return reject(error);
        const std::filesystem::path target = std::filesystem::weakly_canonical(
            tx.stagingDir / relativePath, ec);
        if (ec || !pathWithin(tx.stagingDir, target))
            return reject("archive entry escapes the staging directory");
        if (directoryEntry) {
            ec.clear();
            std::filesystem::create_directories(target, ec);
            if (ec) return reject("could not create archive directory '" + name
                                  + "' in staging (Windows error "
                                  + std::to_string(ec.value()) + ")");
            continue;
        }
        ec.clear();
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) return reject("could not create the parent directory for archive entry '"
                              + name + "' in staging (Windows error "
                              + std::to_string(ec.value()) + ")");
        OutputFile output{std::ofstream(target, std::ios::binary | std::ios::trunc)};
        if (!output.stream || !mz_zip_reader_extract_to_callback(&zip, index, writeExtracted, &output, 0))
            return reject("archive extraction failed");
        // close() flushes the last buffered bytes; a failure there (disk
        // full, AV lock) would otherwise leave a silently truncated file.
        output.stream.close();
        if (!output.stream)
            return reject("could not finish writing archive entry '" + name + "' to staging");
        const DWORD attributes = GetFileAttributesW(target.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return reject("staged output became a reparse point");
        hasLauncher = hasLauncher || folded == "gamehq.exe";
        hasApp = hasApp || folded == "app/gamehq.exe";
        hasManifest = hasManifest || folded == "update-package.json";
    }
    if (!hasLauncher || !hasApp || !hasManifest)
        return reject("archive is missing the required GameHQ layout");
    if (!validateManifest(tx.stagingDir / L"update-package.json", tx, error))
        return reject(error);
    return true;
}
} // namespace updater
