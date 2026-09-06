#include "security/ReleaseTrust.h"

#include <QByteArray>
#include <QDir>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
release_trust::TrustedKey keyFromHex(const char *id, const char *hex,
                                     release_trust::KeyState state = release_trust::KeyState::Current)
{
    release_trust::TrustedKey key;
    key.keyId = id;
    key.state = state;
    const QByteArray bytes = QByteArray::fromHex(hex);
    Q_ASSERT(bytes.size() == 32);
    std::copy(bytes.begin(), bytes.end(), key.publicKey.begin());
    return key;
}

std::vector<std::uint8_t> bytesFromHex(const char *hex)
{
    const QByteArray bytes = QByteArray::fromHex(hex);
    return {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
            reinterpret_cast<const std::uint8_t *>(bytes.constData() + bytes.size())};
}

std::string signatureBase64(const char *hex)
{
    return QByteArray::fromHex(hex).toBase64().toStdString();
}
} // namespace

class ReleaseTrustTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void verifiesRfc8032Vectors();
    void verifiesSharedGameHqVector();
    void rejectsMalformedTamperedAndWrongTrust();
    void enforcesAntiRollback();
    void persistsSequenceAtomically();
    void updaterRunsIndependentSelfTest();
};

void ReleaseTrustTest::verifiesSharedGameHqVector()
{
    QFile file(QStringLiteral(GAMEHQ_RELEASE_VECTOR));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject vector = QJsonDocument::fromJson(file.readAll()).object();
    const QByteArray manifest = QByteArray::fromBase64(
        vector.value(QStringLiteral("manifestBase64")).toString().toLatin1());
    release_trust::TrustedKey key;
    key.keyId = vector.value(QStringLiteral("keyId")).toString().toStdString();
    QVERIFY(release_trust::decodeStrictPublicKeyBase64(
        vector.value(QStringLiteral("publicKeyBase64")).toString().toStdString(), key.publicKey));
    key.state = release_trust::KeyState::Current;
    const std::vector<std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t *>(manifest.constData()),
        reinterpret_cast<const std::uint8_t *>(manifest.constData() + manifest.size()));
    const auto result = release_trust::verify(bytes,
        vector.value(QStringLiteral("signatureBase64")).toString().toStdString(), key.keyId,
        static_cast<std::uint64_t>(vector.value(QStringLiteral("releaseSequence")).toInteger()), {key});
    QVERIFY2(result.accepted(), result.error.c_str());
}

void ReleaseTrustTest::verifiesRfc8032Vectors()
{
    struct Vector { const char *key; const char *message; const char *signature; };
    const Vector vectors[] = {
        {"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
         "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
         "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
         "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"}
    };
    for (int i = 0; i < 3; ++i) {
        const std::string id = "rfc8032-" + std::to_string(i + 1);
        const auto key = keyFromHex(id.c_str(), vectors[i].key);
        const auto result = release_trust::verify(bytesFromHex(vectors[i].message),
            signatureBase64(vectors[i].signature), id, i + 1, {key});
        QVERIFY2(result.accepted(), result.error.c_str());
    }
}

void ReleaseTrustTest::rejectsMalformedTamperedAndWrongTrust()
{
    auto key = keyFromHex("gamehq-test", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    const std::string signature = signatureBase64(
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    const auto message = bytesFromHex("72");

    QCOMPARE(release_trust::verify(message, signature + "\n", key.keyId, 1, {key}).code,
             release_trust::VerifyCode::InvalidSignatureEncoding);
    std::string unpadded = signature.substr(0, signature.size() - 2);
    QCOMPARE(release_trust::verify(message, unpadded, key.keyId, 1, {key}).code,
             release_trust::VerifyCode::InvalidSignatureEncoding);
    auto tampered = message;
    tampered[0] ^= 1;
    QCOMPARE(release_trust::verify(tampered, signature, key.keyId, 1, {key}).code,
             release_trust::VerifyCode::InvalidSignature);
    auto revoked = key;
    revoked.state = release_trust::KeyState::Revoked;
    QCOMPARE(release_trust::verify(message, signature, key.keyId, 1, {revoked}).code,
             release_trust::VerifyCode::RevokedKey);
    auto next = key;
    next.state = release_trust::KeyState::Next;
    QCOMPARE(release_trust::verify(message, signature, key.keyId, 1, {next}).code,
             release_trust::VerifyCode::InactiveKey);
    QCOMPARE(release_trust::verify(message, signature, "unknown", 1, {key}).code,
             release_trust::VerifyCode::UnknownKey);
}

void ReleaseTrustTest::enforcesAntiRollback()
{
    const auto key = keyFromHex("gamehq-test", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    const auto message = bytesFromHex("72");
    const std::string signature = signatureBase64(
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    const auto accepted = release_trust::verify(message, signature, key.keyId, 10, {key});
    QVERIFY(accepted.accepted());
    release_trust::SequenceState state{10, accepted.manifestSha256};
    QVERIFY(release_trust::verify(message, signature, key.keyId, 10, {key}, &state).accepted());
    QCOMPARE(release_trust::verify(message, signature, key.keyId, 9, {key}, &state).code,
             release_trust::VerifyCode::Rollback);
    state.manifestSha256 = std::string(64, '0');
    QCOMPARE(release_trust::verify(message, signature, key.keyId, 10, {key}, &state).code,
             release_trust::VerifyCode::Equivocation);
}

void ReleaseTrustTest::persistsSequenceAtomically()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-release-trust-XXXXXX")));
    QVERIFY(dir.isValid());
    const auto path = std::filesystem::path(dir.filePath(QStringLiteral("release-trust.json")).toStdWString());
    const release_trust::SequenceState expected{42, std::string(64, 'a')};
    std::string error;
    QVERIFY2(release_trust::storeSequenceStateAtomically(path, expected, error), error.c_str());
    release_trust::SequenceState actual;
    QVERIFY2(release_trust::loadSequenceState(path, actual, error), error.c_str());
    QCOMPARE(actual.highestReleaseSequence, expected.highestReleaseSequence);
    QCOMPARE(actual.manifestSha256, expected.manifestSha256);
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("release-trust.json.new"))));
}

void ReleaseTrustTest::updaterRunsIndependentSelfTest()
{
    QProcess process;
    process.start(QStringLiteral(UPDATER_EXE), {QStringLiteral("--release-trust-self-test")});
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitCode(), 0);
    const QByteArray report = process.readAllStandardOutput();
    QVERIFY(report.contains("passed"));

    // The shipped helper must trust production keys only. The tests obtain the
    // RFC 8032 vector key from a separate TEST-ONLY trust library, so no build
    // configuration of this binary - including this one - may report it.
    QVERIFY2(report.contains("TRUST TABLE production"), report.constData());
    QVERIFY2(!report.contains("TRUSTED KEY gamehq-test-"), report.constData());
}

QTEST_GUILESS_MAIN(ReleaseTrustTest)
#include "tst_releasetrust.moc"
