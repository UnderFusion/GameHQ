#include "security/ReleaseTrust.h"
#include "core/DurableFile.h"

#include <monocypher-ed25519.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <system_error>
#include <windows.h>
#include <bcrypt.h>

namespace
{
constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char ch)
{
    const char *found = std::find(std::begin(kBase64Alphabet), std::end(kBase64Alphabet) - 1, ch);
    return found == std::end(kBase64Alphabet) - 1 ? -1 : static_cast<int>(found - kBase64Alphabet);
}

template <std::size_t OutputSize>
bool decodeStrictPaddedBase64(std::string_view encoded, std::array<std::uint8_t, OutputSize> &out)
{
    constexpr std::size_t encodedSize = ((OutputSize + 2) / 3) * 4;
    constexpr std::size_t padding = (3 - (OutputSize % 3)) % 3;
    if (encoded.size() != encodedSize)
        return false;
    for (std::size_t i = 0; i < encoded.size() - padding; ++i) {
        if (base64Value(encoded[i]) < 0)
            return false;
    }
    for (std::size_t i = encoded.size() - padding; i < encoded.size(); ++i) {
        if (encoded[i] != '=')
            return false;
    }

    std::size_t output = 0;
    for (std::size_t input = 0; input < encoded.size(); input += 4) {
        const int a = base64Value(encoded[input]);
        const int b = base64Value(encoded[input + 1]);
        const int c = encoded[input + 2] == '=' ? 0 : base64Value(encoded[input + 2]);
        const int d = encoded[input + 3] == '=' ? 0 : base64Value(encoded[input + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0)
            return false;
        const std::uint32_t word = (static_cast<std::uint32_t>(a) << 18)
            | (static_cast<std::uint32_t>(b) << 12)
            | (static_cast<std::uint32_t>(c) << 6) | static_cast<std::uint32_t>(d);
        if (output < OutputSize) out[output++] = static_cast<std::uint8_t>(word >> 16);
        if (output < OutputSize) out[output++] = static_cast<std::uint8_t>(word >> 8);
        if (output < OutputSize) out[output++] = static_cast<std::uint8_t>(word);
    }
    // Reject non-zero unused bits, which would permit multiple encodings of
    // the same byte string.
    if constexpr (padding == 2) {
        if ((base64Value(encoded[encoded.size() - 3]) & 0x0f) != 0) return false;
    } else if constexpr (padding == 1) {
        if ((base64Value(encoded[encoded.size() - 2]) & 0x03) != 0) return false;
    }
    return output == OutputSize;
}

bool validKeyId(std::string_view keyId)
{
    if (keyId.empty() || keyId.size() > 64)
        return false;
    if (!(keyId.front() >= 'a' && keyId.front() <= 'z') &&
        !(keyId.front() >= '0' && keyId.front() <= '9'))
        return false;
    return std::all_of(keyId.begin(), keyId.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
            || ch == '.' || ch == '_' || ch == '-';
    });
}

bool hexToBytes(std::string_view hex, std::vector<std::uint8_t> &out)
{
    if ((hex.size() % 2) != 0)
        return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = nibble(hex[i]);
        const int low = nibble(hex[i + 1]);
        if (high < 0 || low < 0) return false;
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return true;
}

std::string bytesToBase64(const std::vector<std::uint8_t> &bytes)
{
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t word = static_cast<std::uint32_t>(bytes[i]) << 16
            | (i + 1 < bytes.size() ? static_cast<std::uint32_t>(bytes[i + 1]) << 8 : 0)
            | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out.push_back(kBase64Alphabet[(word >> 18) & 63]);
        out.push_back(kBase64Alphabet[(word >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kBase64Alphabet[(word >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kBase64Alphabet[word & 63] : '=');
    }
    return out;
}
} // namespace

namespace release_trust
{
bool decodeStrictSignatureBase64(std::string_view encoded,
                                 std::array<std::uint8_t, 64> &signatureOut)
{
    return decodeStrictPaddedBase64(encoded, signatureOut);
}

bool decodeStrictPublicKeyBase64(std::string_view encoded,
                                 std::array<std::uint8_t, 32> &keyOut)
{
    return decodeStrictPaddedBase64(encoded, keyOut);
}

std::string sha256Hex(const std::uint8_t *data, std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<ULONG>::max()))
        return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD returned = 0;
    std::array<std::uint8_t, 32> digest{};
    std::vector<std::uint8_t> object;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    const auto closeAlgorithm = [&] { BCryptCloseAlgorithmProvider(algorithm, 0); };
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0) < 0) {
        closeAlgorithm();
        return {};
    }
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0
        || BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) < 0
        || BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        if (hash) BCryptDestroyHash(hash);
        closeAlgorithm();
        return {};
    }
    BCryptDestroyHash(hash);
    closeAlgorithm();
    constexpr char hex[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 15];
    }
    return result;
}

VerifyResult verify(const std::vector<std::uint8_t> &manifestBytes,
                    std::string_view signatureBase64, std::string_view keyId,
                    std::uint64_t releaseSequence,
                    const std::vector<TrustedKey> &trustedKeys,
                    const SequenceState *previousState)
{
    VerifyResult result;
    if (!validKeyId(keyId)) {
        result.code = VerifyCode::InvalidKeyId;
        result.error = "invalid keyId";
        return result;
    }
    const auto key = std::find_if(trustedKeys.begin(), trustedKeys.end(), [&](const TrustedKey &candidate) {
        return candidate.keyId == keyId;
    });
    if (key == trustedKeys.end()) {
        result.code = VerifyCode::UnknownKey;
        result.error = "unknown signing key";
        return result;
    }
    if (key->state == KeyState::Revoked) {
        result.code = VerifyCode::RevokedKey;
        result.error = "revoked signing key";
        return result;
    }
    if (key->state != KeyState::Current || releaseSequence < key->minimumReleaseSequence) {
        result.code = VerifyCode::InactiveKey;
        result.error = "signing key is not active for this sequence";
        return result;
    }
    std::array<std::uint8_t, 64> signature{};
    if (!decodeStrictSignatureBase64(signatureBase64, signature)) {
        result.code = VerifyCode::InvalidSignatureEncoding;
        result.error = "signature is not canonical Base64 for 64 bytes";
        return result;
    }
    const std::uint8_t *message = manifestBytes.empty() ? nullptr : manifestBytes.data();
    if (crypto_ed25519_check(signature.data(), key->publicKey.data(), message,
                             manifestBytes.size()) != 0) {
        result.code = VerifyCode::InvalidSignature;
        result.error = "Ed25519 signature verification failed";
        return result;
    }
    result.manifestSha256 = sha256Hex(message, manifestBytes.size());
    if (result.manifestSha256.empty()) {
        result.code = VerifyCode::StateError;
        result.error = "could not hash the verified manifest";
        return result;
    }
    if (previousState) {
        if (releaseSequence < previousState->highestReleaseSequence) {
            result.code = VerifyCode::Rollback;
            result.error = "release sequence rollback rejected";
            return result;
        }
        if (releaseSequence == previousState->highestReleaseSequence
            && result.manifestSha256 != previousState->manifestSha256) {
            result.code = VerifyCode::Equivocation;
            result.error = "release sequence was reused for different manifest bytes";
            return result;
        }
    }
    result.code = VerifyCode::Accepted;
    return result;
}

bool loadSequenceState(const std::filesystem::path &path, SequenceState &out, std::string &error)
{
    // Runs inside a Qt slot in the app, so only non-throwing overloads here.
    std::error_code ec;
    const bool present = std::filesystem::exists(path, ec);
    if (ec) {
        error = "could not inspect release trust state";
        return false;
    }
    if (!present) {
        out = {};
        return true;
    }
    // The canonical state is about 120 bytes. Anything far larger is corrupt
    // and must not reach the recursive std::regex matcher below.
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > 1024) {
        error = ec ? "could not read release trust state" : "release trust state is corrupt";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "could not read release trust state";
        return false;
    }
    const std::string text = buffer.str();
    const std::regex canonical(R"STATE(\{"schemaVersion":1,"highestReleaseSequence":([0-9]+),"manifestSha256":"([0-9a-f]{64})"\}\n?)STATE");
    std::smatch match;
    bool matched = false;
    try { matched = std::regex_match(text, match, canonical); }
    catch (const std::regex_error &) { matched = false; }
    if (!matched) {
        error = "release trust state is corrupt";
        return false;
    }
    try { out.highestReleaseSequence = std::stoull(match[1].str()); }
    catch (...) { error = "release trust sequence is out of range"; return false; }
    out.manifestSha256 = match[2].str();
    return true;
}

bool storeSequenceStateAtomically(const std::filesystem::path &path,
                                  const SequenceState &state, std::string &error)
{
    if (!std::regex_match(state.manifestSha256, std::regex("[0-9a-f]{64}"))) {
        error = "release trust state has an invalid manifest hash";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = "could not create release trust state directory"; return false; }
    const std::filesystem::path pending = path.wstring() + L".new";
    const std::string text = "{\"schemaVersion\":1,\"highestReleaseSequence\":"
        + std::to_string(state.highestReleaseSequence) + ",\"manifestSha256\":\""
        + state.manifestSha256 + "\"}\n";
    switch (durable_file::replace(path, pending, text)) {
    case durable_file::WriteResult::Written:
        break;
    case durable_file::WriteResult::CreateFailed:
        error = "could not create release trust state";
        return false;
    case durable_file::WriteResult::WriteFailed:
        error = "could not flush release trust state";
        return false;
    case durable_file::WriteResult::PublishFailed:
        error = "could not publish release trust state";
        return false;
    }
    return true;
}

bool runBuiltInSelfTest(std::string &error)
{
    // RFC 8032 section 7.1, test 1: empty message.
    std::vector<std::uint8_t> publicBytes;
    std::vector<std::uint8_t> signatureBytes;
    if (!hexToBytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", publicBytes)
        || !hexToBytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                       "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", signatureBytes)) {
        error = "could not decode built-in RFC 8032 vector";
        return false;
    }
    TrustedKey key;
    key.keyId = "rfc8032-test-1";
    std::copy(publicBytes.begin(), publicBytes.end(), key.publicKey.begin());
    key.state = KeyState::Current;
    const auto result = verify({}, bytesToBase64(signatureBytes), key.keyId, 1, {key});
    if (!result.accepted()) { error = result.error; return false; }
    signatureBytes[0] ^= 1;
    const auto tampered = verify({}, bytesToBase64(signatureBytes), key.keyId, 1, {key});
    if (tampered.code != VerifyCode::InvalidSignature) {
        error = "tampered RFC 8032 signature was accepted";
        return false;
    }
    return true;
}
} // namespace release_trust
