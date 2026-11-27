#include "security/ReleaseManifest.h"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

namespace
{
struct Vector
{
    std::vector<std::uint8_t> manifest;
    std::string signature;
};

std::vector<std::uint8_t> toBytes(const QByteArray &data)
{
    return {reinterpret_cast<const std::uint8_t *>(data.constData()),
            reinterpret_cast<const std::uint8_t *>(data.constData() + data.size())};
}

QByteArray toQByteArray(const std::vector<std::uint8_t> &bytes)
{
    return QByteArray(reinterpret_cast<const char *>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

// The one checked-in fixture that the C++ app, the static helper and the C#
// plugin all verify (docs/release-manifest-security-review.md).
Vector sharedVector()
{
    Vector vector;
    QFile file(QStringLiteral(GAMEHQ_RELEASE_VECTOR));
    if (!file.open(QIODevice::ReadOnly))
        return vector;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    vector.manifest = toBytes(QByteArray::fromBase64(
        object.value(QStringLiteral("manifestBase64")).toString().toLatin1()));
    vector.signature = object.value(QStringLiteral("signatureBase64")).toString().toStdString();
    return vector;
}

// Re-signing is impossible without the private key, so the negative cases that
// need a *valid* signature over *different* content instead assert that the
// altered bytes are rejected. Cases that need different manifest semantics with
// a genuine signature are covered by parse() directly.
std::vector<std::uint8_t> manifestWithReplacement(const std::vector<std::uint8_t> &original,
                                                  const QByteArray &from, const QByteArray &to)
{
    QByteArray text = toQByteArray(original);
    const int at = text.indexOf(from);
    if (at < 0)
        return {};
    text.replace(at, from.size(), to);
    return toBytes(text);
}
} // namespace

class ReleaseManifestTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void acceptsTheSharedVector();
    void rejectsTamperedManifestBytes();
    void rejectsTamperedOrMalformedSignatures();
    void rejectsUntrustedAndRevokedKeys();
    void enforcesAntiRollbackAndEquivocation();
    void rejectsMalformedManifestSemantics();
    void manifestByteLimitIncludesWhitespace();
    void bindsArtifactsToExactBytes();
    void selectsOnlyTheSignedUpdateArtifact();
};

void ReleaseManifestTest::acceptsTheSharedVector()
{
    const Vector vector = sharedVector();
    QVERIFY(!vector.manifest.empty());

    release_manifest::AcceptedRelease accepted;
    std::string error;
    QVERIFY2(release_manifest::verifyAndParse(vector.manifest, vector.signature, nullptr,
                                              accepted, error),
             error.c_str());
    QCOMPARE(accepted.manifest.productId, std::string(release_manifest::kProductId));
    QCOMPARE(accepted.manifest.version, std::string("0.6.24"));
    QCOMPARE(accepted.manifest.releaseSequence, 24ULL);
    QCOMPARE(accepted.keyId, std::string("gamehq-test-2026-01"));
    QCOMPARE(accepted.manifest.keyId, accepted.keyId);
    QCOMPARE(accepted.manifestSha256.size(), std::size_t(64));

    const auto *update = accepted.manifest.artifactOfKind("update");
    QVERIFY(update != nullptr);
    QCOMPARE(update->fileName, std::string("GameHQ-0.6.24-win64-update.zip"));
    QVERIFY(update->size > 0);
}

void ReleaseManifestTest::rejectsTamperedManifestBytes()
{
    const Vector vector = sharedVector();
    QVERIFY(!vector.manifest.empty());
    release_manifest::AcceptedRelease accepted;
    std::string error;

    // Single bit flip anywhere in the signed body.
    for (std::size_t index : {std::size_t(0), vector.manifest.size() / 2,
                              vector.manifest.size() - 1}) {
        std::vector<std::uint8_t> tampered = vector.manifest;
        tampered[index] ^= 0x01;
        QVERIFY(!release_manifest::verifyAndParse(tampered, vector.signature, nullptr,
                                                  accepted, error));
    }

    // Whole-file reshaping the review calls out: BOM, CRLF, added whitespace.
    QByteArray withBom = QByteArrayLiteral("\xEF\xBB\xBF") + toQByteArray(vector.manifest);
    QVERIFY(!release_manifest::verifyAndParse(toBytes(withBom), vector.signature, nullptr,
                                              accepted, error));
    QByteArray crlf = toQByteArray(vector.manifest);
    crlf.replace('\n', "\r\n");
    QVERIFY(!release_manifest::verifyAndParse(toBytes(crlf), vector.signature, nullptr,
                                              accepted, error));
    QByteArray padded = toQByteArray(vector.manifest) + " ";
    QVERIFY(!release_manifest::verifyAndParse(toBytes(padded), vector.signature, nullptr,
                                              accepted, error));

    // Truncation and an oversized body.
    std::vector<std::uint8_t> truncated(vector.manifest.begin(), vector.manifest.end() - 8);
    QVERIFY(!release_manifest::verifyAndParse(truncated, vector.signature, nullptr, accepted, error));
    std::vector<std::uint8_t> oversized(release_manifest::kMaximumManifestBytes + 1, '{');
    QVERIFY(!release_manifest::verifyAndParse(oversized, vector.signature, nullptr, accepted, error));
    QVERIFY(!release_manifest::verifyAndParse({}, vector.signature, nullptr, accepted, error));
}

void ReleaseManifestTest::rejectsTamperedOrMalformedSignatures()
{
    const Vector vector = sharedVector();
    QVERIFY(!vector.manifest.empty());
    release_manifest::AcceptedRelease accepted;
    std::string error;

    // A .sig file legitimately ends with a newline; nothing else is tolerated.
    std::string normalized;
    QVERIFY(release_manifest::normalizeSignatureText(vector.signature + "\n", normalized));
    QCOMPARE(normalized, vector.signature);
    QVERIFY(release_manifest::normalizeSignatureText("  " + vector.signature + " \r\n", normalized));
    QVERIFY(release_manifest::verifyAndParse(vector.manifest, vector.signature + "\r\n", nullptr,
                                             accepted, error));

    // Flip one signature byte: same length, valid Base64, wrong signature.
    QByteArray raw = QByteArray::fromBase64(QByteArray::fromStdString(vector.signature));
    QCOMPARE(raw.size(), 64);
    raw[0] = raw[0] ^ 0x01;
    QVERIFY(!release_manifest::verifyAndParse(vector.manifest, raw.toBase64().toStdString(),
                                              nullptr, accepted, error));
    QByteArray tail = QByteArray::fromBase64(QByteArray::fromStdString(vector.signature));
    tail[63] = tail[63] ^ 0x80;
    QVERIFY(!release_manifest::verifyAndParse(vector.manifest, tail.toBase64().toStdString(),
                                              nullptr, accepted, error));

    // Encoding forms the review forbids.
    std::string ignored;
    QVERIFY(!release_manifest::normalizeSignatureText("", ignored));
    QVERIFY(!release_manifest::normalizeSignatureText(vector.signature.substr(0, 86), ignored));
    QVERIFY(!release_manifest::normalizeSignatureText(vector.signature + "=", ignored));
    QVERIFY(!release_manifest::normalizeSignatureText(vector.signature + "extra", ignored));
    std::string urlSafe = vector.signature;
    std::replace(urlSafe.begin(), urlSafe.end(), '+', '-');
    std::replace(urlSafe.begin(), urlSafe.end(), '/', '_');
    if (urlSafe != vector.signature)
        QVERIFY(!release_manifest::normalizeSignatureText(urlSafe, ignored));
    std::string spaced = vector.signature;
    spaced.insert(spaced.size() / 2, " ");
    QVERIFY(!release_manifest::normalizeSignatureText(spaced, ignored));
    QVERIFY(!release_manifest::normalizeSignatureText(
        std::string(release_manifest::kMaximumSignatureBytes + 1, 'A'), ignored));
}

void ReleaseManifestTest::rejectsUntrustedAndRevokedKeys()
{
    const Vector vector = sharedVector();
    QVERIFY(!vector.manifest.empty());
    release_manifest::AcceptedRelease accepted;
    std::string error;

    // The signature stays valid but the manifest now names a key the shipped
    // table does not contain: verification must fail because manifest data can
    // never select or create trust.
    const auto renamed = manifestWithReplacement(vector.manifest,
                                                 "gamehq-test-2026-01", "gamehq-test-2099-99");
    QVERIFY(!renamed.empty());
    QVERIFY(!release_manifest::verifyAndParse(renamed, vector.signature, nullptr, accepted, error));

    // Production trust is always present. Test builds additionally ship the
    // public RFC vector so shared fixtures remain executable.
    QVERIFY(!release_manifest::trustedKeys().empty());
    QVERIFY(!release_manifest::trustTableIsTestOnly());
    const auto production = std::find_if(
        release_manifest::trustedKeys().cbegin(),
        release_manifest::trustedKeys().cend(),
        [](const release_trust::TrustedKey &key) {
            return key.keyId == "gamehq-prod-2026-01";
        });
    QVERIFY(production != release_manifest::trustedKeys().cend());
    QCOMPARE(production->state, release_trust::KeyState::Current);
    QCOMPARE(production->minimumReleaseSequence, 25ULL);

    // Revoked and next-state keys never verify, even with a good signature.
    release_trust::TrustedKey revoked = release_manifest::trustedKeys().front();
    revoked.state = release_trust::KeyState::Revoked;
    QCOMPARE(release_trust::verify(vector.manifest, vector.signature, revoked.keyId, 24,
                                   {revoked}).code,
             release_trust::VerifyCode::RevokedKey);
    release_trust::TrustedKey next = release_manifest::trustedKeys().front();
    next.state = release_trust::KeyState::Next;
    QCOMPARE(release_trust::verify(vector.manifest, vector.signature, next.keyId, 24, {next}).code,
             release_trust::VerifyCode::InactiveKey);

    // A key that is current but not yet active for this sequence.
    release_trust::TrustedKey future = release_manifest::trustedKeys().front();
    future.minimumReleaseSequence = 999;
    QCOMPARE(release_trust::verify(vector.manifest, vector.signature, future.keyId, 24,
                                   {future}).code,
             release_trust::VerifyCode::InactiveKey);
}

void ReleaseManifestTest::enforcesAntiRollbackAndEquivocation()
{
    const Vector vector = sharedVector();
    QVERIFY(!vector.manifest.empty());
    release_manifest::AcceptedRelease accepted;
    std::string error;
    QVERIFY(release_manifest::verifyAndParse(vector.manifest, vector.signature, nullptr,
                                             accepted, error));

    // Replaying the same release is idempotent.
    release_trust::SequenceState state;
    state.highestReleaseSequence = accepted.manifest.releaseSequence;
    state.manifestSha256 = accepted.manifestSha256;
    QVERIFY(release_manifest::verifyAndParse(vector.manifest, vector.signature, &state,
                                            accepted, error));

    // A higher stored sequence means this manifest is a rollback.
    release_trust::SequenceState newer;
    newer.highestReleaseSequence = accepted.manifest.releaseSequence + 1;
    newer.manifestSha256 = accepted.manifestSha256;
    QVERIFY(!release_manifest::verifyAndParse(vector.manifest, vector.signature, &newer,
                                             accepted, error));

    // Same sequence, different bytes: equivocation.
    release_trust::SequenceState equivocating;
    equivocating.highestReleaseSequence = 24;
    equivocating.manifestSha256 = std::string(64, 'a');
    QVERIFY(!release_manifest::verifyAndParse(vector.manifest, vector.signature, &equivocating,
                                             accepted, error));
}

void ReleaseManifestTest::rejectsMalformedManifestSemantics()
{
    // parse() runs only on already-verified bytes, so these cases model a
    // future key signing a manifest that violates the schema.
    const auto check = [](const QByteArray &json) {
        release_manifest::Manifest manifest;
        std::string error;
        return release_manifest::parse(toBytes(json), manifest, error);
    };
    const QByteArray valid = R"({"schemaVersion":1,"productId":"underfusion.gamehq","version":"1.2.3",)"
        R"("releaseSequence":7,"publishedAtUtc":"2026-07-22T00:00:00Z","minimumUpdaterVersion":"1.0.0",)"
        R"("artifacts":[{"kind":"update","fileName":"GameHQ-1.2.3-win64-update.zip","size":10,)"
        R"("sha256":"0000000000000000000000000000000000000000000000000000000000000000"}],)"
        R"("keyId":"gamehq-test-2026-01"})";
    QVERIFY(check(valid));

    QByteArray wrongProduct = valid;
    wrongProduct.replace("underfusion.gamehq", "underfusion.other");
    QVERIFY(!check(wrongProduct));

    QByteArray wrongSchema = valid;
    wrongSchema.replace("\"schemaVersion\":1", "\"schemaVersion\":2");
    QVERIFY(!check(wrongSchema));

    QByteArray zeroSequence = valid;
    zeroSequence.replace("\"releaseSequence\":7", "\"releaseSequence\":0");
    QVERIFY(!check(zeroSequence));

    QByteArray badVersion = valid;
    badVersion.replace("\"version\":\"1.2.3\"", "\"version\":\"1.2\"");
    QVERIFY(!check(badVersion));

    QByteArray badTimestamp = valid;
    badTimestamp.replace("2026-07-22T00:00:00Z", "2026-07-22 00:00:00");
    QVERIFY(!check(badTimestamp));

    QByteArray badHash = valid;
    badHash.replace(QByteArray(64, '0'), QByteArray(64, 'Z'));
    QVERIFY(!check(badHash));

    // A file name that does not carry the released version cannot be
    // authorised: this is the "signed name from another release" case.
    QByteArray foreignAsset = valid;
    foreignAsset.replace("GameHQ-1.2.3-win64-update.zip", "GameHQ-9.9.9-win64-update.zip");
    QVERIFY(!check(foreignAsset));

    // Path traversal through the artifact name.
    QByteArray traversal = valid;
    traversal.replace("GameHQ-1.2.3-win64-update.zip", "..1.2.3/evil.zip");
    QVERIFY(!check(traversal));

    // Structural abuse.
    QVERIFY(!check(valid + " trailing"));
    QVERIFY(!check(QByteArrayLiteral("{}")));
    QVERIFY(!check(QByteArrayLiteral("not json")));
    QByteArray duplicated = valid;
    duplicated.replace("\"keyId\":", "\"version\":\"1.2.3\",\"keyId\":");
    QVERIFY(!check(duplicated));
    QByteArray unknownField = valid;
    unknownField.replace("\"keyId\":", "\"surprise\":1,\"keyId\":");
    QVERIFY(!check(unknownField));
    QByteArray floatSize = valid;
    floatSize.replace("\"size\":10", "\"size\":10.5");
    QVERIFY(!check(floatSize));
    QByteArray negativeSize = valid;
    negativeSize.replace("\"size\":10", "\"size\":-1");
    QVERIFY(!check(negativeSize));
    QByteArray noArtifacts = valid;
    noArtifacts.replace(noArtifacts.mid(noArtifacts.indexOf("\"artifacts\":["),
                                        noArtifacts.indexOf("],") + 2
                                            - noArtifacts.indexOf("\"artifacts\":[")),
                        "\"artifacts\":[],");
    QVERIFY(!check(noArtifacts));
}

void ReleaseManifestTest::manifestByteLimitIncludesWhitespace()
{
    const Vector vector = sharedVector();
    QVERIFY(!vector.manifest.empty());
    QCOMPARE(release_manifest::kMaximumManifestBytes, std::size_t(65536));

    // Whitespace is valid JSON, and the bound applies to bytes before parsing.
    // Reformatting must never make the original detached signature reusable.
    QByteArray padded = toQByteArray(vector.manifest);
    padded = padded.leftJustified(qsizetype(release_manifest::kMaximumManifestBytes), ' ');
    release_manifest::Manifest parsed;
    std::string error;
    QVERIFY2(release_manifest::parse(toBytes(padded), parsed, error), error.c_str());
    QCOMPARE(parsed.releaseSequence, 24ULL);

    release_manifest::AcceptedRelease accepted;
    QVERIFY(!release_manifest::verifyAndParse(toBytes(padded), vector.signature, nullptr,
                                               accepted, error));
    padded.append(' ');
    QVERIFY(!release_manifest::parse(toBytes(padded), parsed, error));
}

void ReleaseManifestTest::bindsArtifactsToExactBytes()
{
    release_manifest::Artifact artifact;
    artifact.kind = "update";
    artifact.fileName = "GameHQ-1.2.3-win64-update.zip";
    const std::vector<std::uint8_t> payload{'G', 'a', 'm', 'e', 'H', 'Q'};
    artifact.size = payload.size();
    artifact.sha256 = release_trust::sha256Hex(payload.data(), payload.size());
    QCOMPARE(artifact.sha256.size(), std::size_t(64));

    std::string error;
    QVERIFY(release_manifest::artifactMatchesFile(artifact, payload, error));

    // Correct name and size, different content: the checksum asset alone would
    // have accepted this if it were replaced alongside the archive.
    std::vector<std::uint8_t> swapped = payload;
    swapped[0] = 'g';
    QVERIFY(!release_manifest::artifactMatchesFile(artifact, swapped, error));

    // Correct hash source but wrong length.
    std::vector<std::uint8_t> shortened(payload.begin(), payload.end() - 1);
    QVERIFY(!release_manifest::artifactMatchesFile(artifact, shortened, error));
    std::vector<std::uint8_t> extended = payload;
    extended.push_back('!');
    QVERIFY(!release_manifest::artifactMatchesFile(artifact, extended, error));
}

void ReleaseManifestTest::selectsOnlyTheSignedUpdateArtifact()
{
    const Vector vector = sharedVector();
    release_manifest::AcceptedRelease accepted;
    std::string error;
    QVERIFY(release_manifest::verifyAndParse(vector.manifest, vector.signature, nullptr,
                                             accepted, error));

    QVERIFY(accepted.manifest.artifactOfKind("update") != nullptr);
    QVERIFY(accepted.manifest.artifactOfKind("setup") != nullptr);
    QVERIFY(accepted.manifest.artifactOfKind("portable") != nullptr);
    QCOMPARE(accepted.manifest.artifactOfKind("installer"), nullptr);
    QCOMPARE(accepted.manifest.artifactOfKind(""), nullptr);

    // Every artifact belongs to the signed version, so a GitHub asset from a
    // different release can never match by name.
    for (const auto &artifact : accepted.manifest.artifacts)
        QVERIFY(artifact.fileName.find(accepted.manifest.version) != std::string::npos);
}

QTEST_MAIN(ReleaseManifestTest)
#include "tst_releasemanifest.moc"
