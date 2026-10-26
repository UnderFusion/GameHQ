#include "updater/UpdaterTransaction.h"

#include "core/PathContainment.h"
#include "security/ReleaseManifest.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <system_error>
#include <variant>
#include <windows.h>
#include <shlobj.h>

namespace
{
using JsonValue = std::variant<std::string, long long>;

void appendUtf8(std::string &out, unsigned int codePoint)
{
    if (codePoint <= 0x7f) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else if (codePoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
}

class FlatJsonParser
{
public:
    explicit FlatJsonParser(const std::string &text) : m_text(text) {}

    bool parse(std::map<std::string, JsonValue> &values, std::string &error)
    {
        skipSpace();
        if (!take('{'))
            return fail(error, "transaction must be a JSON object");
        skipSpace();
        if (take('}'))
            return finish(error);
        for (;;) {
            std::string key;
            if (!parseString(key, error))
                return false;
            skipSpace();
            if (!take(':'))
                return fail(error, "expected ':' after transaction field");
            skipSpace();
            JsonValue value;
            if (peek() == '"') {
                std::string stringValue;
                if (!parseString(stringValue, error))
                    return false;
                value = std::move(stringValue);
            } else {
                long long number = 0;
                if (!parseInteger(number, error))
                    return false;
                value = number;
            }
            if (!values.emplace(std::move(key), std::move(value)).second)
                return fail(error, "transaction contains a duplicate field");
            skipSpace();
            if (take('}'))
                return finish(error);
            if (!take(','))
                return fail(error, "expected ',' between transaction fields");
            skipSpace();
        }
    }

private:
    char peek() const { return m_pos < m_text.size() ? m_text[m_pos] : '\0'; }
    bool take(char expected)
    {
        if (peek() != expected)
            return false;
        ++m_pos;
        return true;
    }
    void skipSpace()
    {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
            ++m_pos;
    }
    bool finish(std::string &error)
    {
        skipSpace();
        return m_pos == m_text.size() || fail(error, "unexpected data after transaction object");
    }
    bool fail(std::string &error, const char *message)
    {
        error = message;
        return false;
    }
    bool parseHex4(unsigned int &value, std::string &error)
    {
        if (m_text.size() - m_pos < 4)
            return fail(error, "incomplete Unicode escape in transaction");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = m_text[m_pos++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') value |= static_cast<unsigned int>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') value |= static_cast<unsigned int>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') value |= static_cast<unsigned int>(ch - 'A' + 10);
            else return fail(error, "invalid Unicode escape in transaction");
        }
        return true;
    }
    bool parseString(std::string &out, std::string &error)
    {
        if (!take('"'))
            return fail(error, "expected a quoted transaction field");
        while (m_pos < m_text.size()) {
            const unsigned char ch = static_cast<unsigned char>(m_text[m_pos++]);
            if (ch == '"')
                return true;
            if (ch < 0x20)
                return fail(error, "control character in transaction string");
            if (ch != '\\') {
                out.push_back(static_cast<char>(ch));
                continue;
            }
            if (m_pos >= m_text.size())
                return fail(error, "incomplete escape in transaction string");
            const char escaped = m_text[m_pos++];
            switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned int codePoint = 0;
                if (!parseHex4(codePoint, error))
                    return false;
                if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
                    if (m_text.size() - m_pos < 6 || m_text[m_pos] != '\\' || m_text[m_pos + 1] != 'u')
                        return fail(error, "unpaired Unicode surrogate in transaction");
                    m_pos += 2;
                    unsigned int low = 0;
                    if (!parseHex4(low, error) || low < 0xdc00 || low > 0xdfff)
                        return fail(error, "invalid Unicode surrogate pair in transaction");
                    codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
                    return fail(error, "unpaired Unicode surrogate in transaction");
                }
                appendUtf8(out, codePoint);
                break;
            }
            default: return fail(error, "invalid escape in transaction string");
            }
        }
        return fail(error, "unterminated transaction string");
    }
    bool parseInteger(long long &out, std::string &error)
    {
        const std::size_t begin = m_pos;
        if (peek() == '-')
            ++m_pos;
        while (std::isdigit(static_cast<unsigned char>(peek())))
            ++m_pos;
        if (m_pos == begin || (m_pos == begin + 1 && m_text[begin] == '-'))
            return fail(error, "transaction values must be strings or integers");
        try {
            out = std::stoll(m_text.substr(begin, m_pos - begin));
        } catch (...) {
            return fail(error, "transaction integer is out of range");
        }
        return true;
    }

    const std::string &m_text;
    std::size_t m_pos = 0;
};

bool utf8ToPath(const std::string &value, std::filesystem::path &pathOut)
{
    if (value.empty())
        return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return false;
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), wide.data(), length) != length)
        return false;
    pathOut = std::filesystem::path(wide);
    return true;
}

bool getString(const std::map<std::string, JsonValue> &values, const char *key,
               std::string &out, std::string &error)
{
    const auto it = values.find(key);
    if (it == values.end() || !std::holds_alternative<std::string>(it->second)) {
        error = std::string("missing or invalid transaction field: ") + key;
        return false;
    }
    out = std::get<std::string>(it->second);
    return !out.empty() || (error = std::string("empty transaction field: ") + key, false);
}

bool getPath(const std::map<std::string, JsonValue> &values, const char *key,
             std::filesystem::path &out, std::string &error)
{
    std::string utf8;
    if (!getString(values, key, utf8, error))
        return false;
    if (!utf8ToPath(utf8, out)) {
        error = std::string("invalid UTF-8 path in transaction field: ") + key;
        return false;
    }
    return true;
}

std::filesystem::path normalizedPath(const std::filesystem::path &path, std::string &error)
{
    if (!path.is_absolute()) {
        error = "transaction paths must be absolute";
        return {};
    }
    std::error_code ec;
    const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        error = "could not canonicalize transaction path";
        return {};
    }
    return normalized;
}

bool pathWithin(const std::filesystem::path &root, const std::filesystem::path &candidate,
                bool allowRoot)
{
    return path_containment::isWithinRoot(root, candidate, allowRoot);
}

std::filesystem::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
        std::error_code ec;
        result = std::filesystem::weakly_canonical(std::filesystem::path(raw), ec);
        if (ec)
            result.clear();
    }
    CoTaskMemFree(raw);
    return result;
}

// The helper snapshots dataDir and restores it on rollback, so it must name a
// real GameHQ data location: below the package root (portable) or below the
// user's own AppData (installed; Qt's AppDataLocation and the legacy
// GenericDataLocation copies). It may never cover the program files or the
// update working area it would otherwise restore over.
bool dataDirAllowed(const updater::Transaction &tx)
{
    const std::filesystem::path updateArea = tx.healthTokenPath.parent_path();
    if (pathWithin(tx.dataDir, tx.packageRoot, true) || pathWithin(updateArea, tx.dataDir, true))
        return false;
    if (pathWithin(tx.packageRoot, tx.dataDir, false))
        return true;
    for (const auto &root : { knownFolder(FOLDERID_RoamingAppData),
                              knownFolder(FOLDERID_LocalAppData) }) {
        if (!root.empty() && pathWithin(root, tx.dataDir, false))
            return true;
    }
    return false;
}
} // namespace

namespace updater
{
std::string pathToUtf8(const std::filesystem::path &path)
{
    const std::wstring wide = path.wstring();
    if (wide.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

bool loadAndValidateTransaction(const std::filesystem::path &transactionPath,
                                Transaction &out, std::string &error)
{
    // Several later checks only fill in a default reason when error is still
    // empty, so a message left over from a previous call would be reported as
    // this call's failure.
    error.clear();
    std::ifstream input(transactionPath, std::ios::binary);
    if (!input) {
        error = "could not open update transaction";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "could not read update transaction";
        return false;
    }
    const std::string json = buffer.str();
    if (json.size() > 64 * 1024) {
        error = "update transaction exceeds 64 KiB";
        return false;
    }

    std::map<std::string, JsonValue> values;
    if (!FlatJsonParser(json).parse(values, error))
        return false;
    const auto schema = values.find("schemaVersion");
    if (schema == values.end() || !std::holds_alternative<long long>(schema->second)
        || (std::get<long long>(schema->second) != 1 && std::get<long long>(schema->second) != 2)) {
        error = "unsupported update transaction schema";
        return false;
    }
    out.schemaVersion = static_cast<int>(std::get<long long>(schema->second));
    if (!getString(values, "productId", out.productId, error)
        || !getString(values, "expectedVersion", out.expectedVersion, error)
        || !getString(values, "expectedSha256", out.expectedSha256, error)
        || !getPath(values, "packageRoot", out.packageRoot, error)
        || !getPath(values, "packagePath", out.packagePath, error)
        || !getPath(values, "stagingDir", out.stagingDir, error)
        || !getPath(values, "backupDir", out.backupDir, error)
        || !getPath(values, "restartExecutable", out.restartExecutable, error)
        || !getPath(values, "healthTokenPath", out.healthTokenPath, error)
        || !getPath(values, "dataDir", out.dataDir, error)
        || !getPath(values, "dataSnapshotDir", out.dataSnapshotDir, error)
        || !getPath(values, "manifestPath", out.manifestPath, error)
        || !getPath(values, "signaturePath", out.signaturePath, error)
        || !getString(values, "manifestSha256", out.manifestSha256, error)
        || !getString(values, "releaseSignature", out.releaseSignature, error)
        || !getString(values, "releaseKeyId", out.releaseKeyId, error)
        || !getString(values, "artifactName", out.artifactName, error)
        || !getString(values, "artifactSha256", out.artifactSha256, error)
        || !getString(values, "phase", out.phase, error))
        return false;
    const auto releaseSequence = values.find("releaseSequence");
    if (releaseSequence == values.end() || !std::holds_alternative<long long>(releaseSequence->second)
        || std::get<long long>(releaseSequence->second) <= 0) {
        error = "missing or invalid transaction field: releaseSequence";
        return false;
    }
    out.releaseSequence = static_cast<unsigned long long>(std::get<long long>(releaseSequence->second));
    const auto artifactSize = values.find("artifactSize");
    if (artifactSize == values.end() || !std::holds_alternative<long long>(artifactSize->second)
        || std::get<long long>(artifactSize->second) <= 0) {
        error = "missing or invalid transaction field: artifactSize";
        return false;
    }
    out.artifactSize = std::get<long long>(artifactSize->second);
    const auto callerPid = values.find("callerPid");
    if (callerPid == values.end() || !std::holds_alternative<long long>(callerPid->second)
        || std::get<long long>(callerPid->second) <= 0) {
        error = "missing or invalid transaction field: callerPid";
        return false;
    }
    out.callerPid = std::get<long long>(callerPid->second);
    // Schema 2 pins the handle to one exact process. Schema 1 predates that and
    // is refused by every mutating mode in UpdaterMain.
    const auto creationTime = values.find("callerCreationTime");
    if (out.schemaVersion >= 2) {
        if (creationTime == values.end() || !std::holds_alternative<long long>(creationTime->second)
            || std::get<long long>(creationTime->second) <= 0) {
            error = "missing or invalid transaction field: callerCreationTime";
            return false;
        }
        out.callerCreationTime =
            static_cast<unsigned long long>(std::get<long long>(creationTime->second));
    } else if (creationTime != values.end()) {
        error = "schema 1 transaction must not carry a caller creation time";
        return false;
    }

    if (out.productId != "underfusion.gamehq") {
        error = "transaction product does not match GameHQ";
        return false;
    }
    if (!std::regex_match(out.expectedVersion, std::regex(R"([0-9]+\.[0-9]+\.[0-9]+)"))) {
        error = "transaction contains an invalid target version";
        return false;
    }
    if (!std::regex_match(out.expectedSha256, std::regex(R"([0-9A-Fa-f]{64})"))) {
        error = "transaction contains an invalid SHA-256 digest";
        return false;
    }
    std::transform(out.expectedSha256.begin(), out.expectedSha256.end(),
                   out.expectedSha256.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (out.phase != "download_verified") {
        error = "transaction is not ready for updater validation";
        return false;
    }

    out.packageRoot = normalizedPath(out.packageRoot, error);
    if (out.packageRoot.empty() || !std::filesystem::is_directory(out.packageRoot)) {
        if (error.empty()) error = "package root does not exist";
        return false;
    }
    std::filesystem::path transaction = normalizedPath(transactionPath, error);
    out.packagePath = normalizedPath(out.packagePath, error);
    out.stagingDir = normalizedPath(out.stagingDir, error);
    out.backupDir = normalizedPath(out.backupDir, error);
    out.restartExecutable = normalizedPath(out.restartExecutable, error);
    out.healthTokenPath = normalizedPath(out.healthTokenPath, error);
    out.dataDir = normalizedPath(out.dataDir, error);
    out.dataSnapshotDir = normalizedPath(out.dataSnapshotDir, error);
    if (!error.empty())
        return false;
    for (const auto &candidate : { transaction, out.packagePath, out.stagingDir, out.backupDir,
                                   out.restartExecutable, out.healthTokenPath, out.dataSnapshotDir }) {
        if (!pathWithin(out.packageRoot, candidate, false)) {
            error = "transaction path escapes the package root: " + pathToUtf8(candidate);
            return false;
        }
    }
    if (!dataDirAllowed(out)) {
        error = "transaction data directory is not a GameHQ data location: " + pathToUtf8(out.dataDir);
        return false;
    }
    if (out.stagingDir == out.backupDir) {
        error = "staging and backup directories must be different";
        return false;
    }
    const std::string expectedName = "GameHQ-" + out.expectedVersion + "-win64-update.zip";
    if (pathToUtf8(out.packagePath.filename()) != expectedName) {
        error = "transaction package name does not match the target version";
        return false;
    }
    if (!std::filesystem::is_regular_file(out.packagePath)) {
        error = "verified update package is missing";
        return false;
    }
    if (!std::regex_match(out.manifestSha256, std::regex(R"([0-9a-f]{64})"))
        || !std::regex_match(out.artifactSha256, std::regex(R"([0-9a-f]{64})"))) {
        error = "transaction contains an invalid release manifest digest";
        return false;
    }
    if (out.artifactName != expectedName) {
        error = "transaction artifact name does not match the target version";
        return false;
    }
    for (const auto &candidate : { out.manifestPath, out.signaturePath }) {
        if (!pathWithin(out.packageRoot, candidate, false)) {
            error = "release manifest path escapes the package root: " + pathToUtf8(candidate);
            return false;
        }
        if (!std::filesystem::is_regular_file(candidate)) {
            error = "release manifest evidence is missing: " + pathToUtf8(candidate);
            return false;
        }
    }
    // The decisive check: the manifest on disk must still verify and must still
    // authorise exactly this package.
    if (!verifyReleaseAuthorisation(out, error))
        return false;
    return true;
}

void *openAuthorisingCaller(const Transaction &tx, bool &alreadyExited, std::string &error)
{
    alreadyExited = false;
    if (tx.schemaVersion < 2 || tx.callerCreationTime == 0) {
        error = "this transaction does not identify its caller precisely enough";
        return nullptr;
    }
    if (tx.callerPid <= 0 || tx.callerPid > 0xffffffffLL) {
        error = "transaction caller process id is out of range";
        return nullptr;
    }
    // QUERY_LIMITED_INFORMATION is what GetProcessTimes needs and is the least
    // privilege that still proves identity.
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(tx.callerPid));
    if (!process) {
        const DWORD lastError = GetLastError();
        if (lastError == ERROR_INVALID_PARAMETER) {
            // No process with that id exists any more. The caller cannot still
            // be holding files, so this is a normal early-exit, not a failure.
            alreadyExited = true;
            return nullptr;
        }
        error = "could not open the application that authorised this update (error "
            + std::to_string(lastError) + ")";
        return nullptr;
    }

    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        CloseHandle(process);
        error = "could not read the authorising process creation time";
        return nullptr;
    }
    const unsigned long long actual = (static_cast<unsigned long long>(created.dwHighDateTime) << 32)
        | created.dwLowDateTime;
    if (actual != tx.callerCreationTime) {
        // The id was reused by an unrelated process; waiting on it would be
        // meaningless and could let the real caller keep running.
        CloseHandle(process);
        error = "the authorising application is gone and its process id was reused";
        return nullptr;
    }
    return process;
}

bool verifyReleaseAuthorisation(const Transaction &tx, std::string &error)
{
    const auto readFile = [](const std::filesystem::path &path, std::size_t limit,
                             std::vector<std::uint8_t> &out) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size == 0 || size > limit)
            return false;
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return false;
        out.resize(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
        return static_cast<std::size_t>(input.gcount()) == out.size();
    };

    std::vector<std::uint8_t> manifestBytes;
    if (!readFile(tx.manifestPath, release_manifest::kMaximumManifestBytes, manifestBytes)) {
        error = "could not read the release manifest";
        return false;
    }
    std::vector<std::uint8_t> signatureBytes;
    if (!readFile(tx.signaturePath, release_manifest::kMaximumSignatureBytes, signatureBytes)) {
        error = "could not read the release signature";
        return false;
    }
    // The transaction's own copy of the signature must agree with the file, so
    // neither can be swapped on its own.
    const std::string signatureText(reinterpret_cast<const char *>(signatureBytes.data()),
                                    signatureBytes.size());
    std::string canonicalFromFile;
    if (!release_manifest::normalizeSignatureText(signatureText, canonicalFromFile)
        || canonicalFromFile != tx.releaseSignature) {
        error = "the release signature does not match the transaction";
        return false;
    }
    if (release_trust::sha256Hex(manifestBytes.data(), manifestBytes.size()) != tx.manifestSha256) {
        error = "the release manifest does not match the transaction";
        return false;
    }

    // Stateless: the helper must never advance the anti-rollback counter, only
    // the app that actually accepted the release does that.
    release_manifest::AcceptedRelease accepted;
    if (!release_manifest::verifyAndParse(manifestBytes, signatureText, nullptr, accepted, error))
        return false;
    if (accepted.keyId != tx.releaseKeyId || accepted.manifest.releaseSequence != tx.releaseSequence) {
        error = "the release manifest was signed by a different key or sequence";
        return false;
    }
    if (accepted.manifest.version != tx.expectedVersion) {
        error = "the release manifest authorises a different version";
        return false;
    }
    const release_manifest::Artifact *update = accepted.manifest.artifactOfKind("update");
    if (!update) {
        error = "the release manifest authorises no update package";
        return false;
    }
    if (update->fileName != tx.artifactName
        || static_cast<long long>(update->size) != tx.artifactSize
        || update->sha256 != tx.artifactSha256 || update->sha256 != tx.expectedSha256) {
        error = "the release manifest does not authorise this package";
        return false;
    }
    std::error_code ec;
    const auto actualSize = std::filesystem::file_size(tx.packagePath, ec);
    if (ec || static_cast<long long>(actualSize) != tx.artifactSize) {
        error = "the staged package length does not match the signed manifest";
        return false;
    }
    return true;
}

std::vector<std::string> plannedOperations(const Transaction &tx)
{
    const std::vector<std::string> ownedPaths = {
        "GameHQ.exe", "app/", "README.txt", "LICENSE.txt",
        "THIRD_PARTY_NOTICES.md", "licenses/",
        "GameHQUpdater.pending.exe"
    };
    std::vector<std::string> operations;
    operations.push_back("WAIT FOR CALLER EXIT pid=" + std::to_string(tx.callerPid));
    operations.push_back("VERIFY SHA-256 " + pathToUtf8(tx.packagePath));
    operations.push_back("CREATE STAGING " + pathToUtf8(tx.stagingDir));
    operations.push_back("EXTRACT PACKAGE " + pathToUtf8(tx.packagePath) + " -> " + pathToUtf8(tx.stagingDir));
    operations.push_back("CREATE BACKUP " + pathToUtf8(tx.backupDir));
    operations.push_back("SNAPSHOT DATA " + pathToUtf8(tx.dataDir) + " -> " + pathToUtf8(tx.dataSnapshotDir));
    for (const std::string &path : ownedPaths) {
        operations.push_back("BACKUP IF PRESENT " + path);
        operations.push_back("INSTALL IF PRESENT " + path);
    }
    operations.push_back("LAUNCH " + pathToUtf8(tx.restartExecutable)
                         + " --post-update " + tx.expectedVersion + " " + pathToUtf8(tx.healthTokenPath));
    return operations;
}
} // namespace updater
