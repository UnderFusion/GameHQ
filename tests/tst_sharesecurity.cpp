#include "share/ShareSecurity.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace share;

// Share Platform security boundaries (t11). The secret store test writes to
// the real Windows Credential Manager under a throwaway namespace and removes
// everything it wrote, so it leaves no trace in the user's credentials.
class TestShareSecurity : public QObject
{
    Q_OBJECT

private slots:
    void redactsKnownSecretShapes_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("mustNotContain");

        QTest::newRow("discord webhook")
            << "POST https://discord.com/api/webhooks/123456789012345678/AbC-dEf_ghIJKlmnopQRstuVWxyz0123456789abcdEFGH failed"
            << "AbC-dEf_ghIJKlmnopQRstuVWxyz0123456789abcdEFGH";
        QTest::newRow("discordapp ptb webhook")
            << "https://ptb.discordapp.com/api/v10/webhooks/1/tok.en-value"
            << "tok.en-value";
        QTest::newRow("telegram bot token")
            << "bot 123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw0 rejected"
            << "AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw0";
        QTest::newRow("bearer")
            << "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig"
            << "eyJhbGciOiJIUzI1NiJ9";
        QTest::newRow("query token")
            << "https://example.com/upload?user=me&access_token=abc123secret&x=1"
            << "abc123secret";
        QTest::newRow("json token")
            << "{\"ok\":false,\"token\":\"s3cr3t-value\"}"
            << "s3cr3t-value";
        QTest::newRow("long opaque run")
            << "session key 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
            << "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
    }

    void redactsKnownSecretShapes()
    {
        QFETCH(QString, input);
        QFETCH(QString, mustNotContain);
        const QString out = redactSecrets(input);
        QVERIFY2(!out.contains(mustNotContain), qPrintable(out));
        QVERIFY(out.contains(QStringLiteral("<redacted>")));
        QCOMPARE(redactSecrets(out), out);   // idempotent
    }

    void leavesOrdinaryTextAlone()
    {
        const QString text = QStringLiteral("Upload failed: rate_limited (retry after 2 s) for 2026-09-29_14-00-00.jpg");
        QCOMPARE(redactSecrets(text), text);
        // The webhook id stays for support, only the token goes.
        QVERIFY(redactSecrets(QStringLiteral("https://discord.com/api/webhooks/42/secret-token-value"))
                    .contains(QStringLiteral("/webhooks/42/")));
    }

    void secretStoreRoundTrip()
    {
        SecretStore store(QStringLiteral("GameHQ.Test.") + QUuid::createUuid().toString(QUuid::Id128));
        const QString provider = QStringLiteral("discord.webhook");
        bool found = true;
        QVERIFY(store.read(provider, QStringLiteral("channel-1"), &found).isEmpty());
        QVERIFY(!found);

        QVERIFY(store.write(provider, QStringLiteral("channel-1"), "https://discord.com/api/webhooks/1/a"));
        QVERIFY(store.write(provider, QStringLiteral("channel-2"), "https://discord.com/api/webhooks/2/b"));
        QCOMPARE(store.read(provider, QStringLiteral("channel-1"), &found),
                 QByteArray("https://discord.com/api/webhooks/1/a"));
        QVERIFY(found);
        QStringList names = store.names(provider);
        names.sort();
        QCOMPARE(names, (QStringList{ QStringLiteral("channel-1"), QStringLiteral("channel-2") }));

        // Invalid ids/names and oversized secrets are refused, not truncated.
        QVERIFY(!store.write(QStringLiteral("Bad Id"), QStringLiteral("x"), "v"));
        QVERIFY(!store.write(provider, QStringLiteral("../escape"), "v"));
        QVERIFY(!store.write(provider, QStringLiteral("big"), QByteArray(SecretStore::kMaxSecretBytes + 1, 'x')));

        QCOMPARE(store.removeAll(provider), 2);
        QVERIFY(store.names(provider).isEmpty());
        QVERIFY(store.read(provider, QStringLiteral("channel-2"), &found).isEmpty());
        QVERIFY(!found);
    }

    void sessionStorageStaysInsideItsRoot()
    {
        QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("share"));
        SessionStorage storage(root);
        const QString sessionDir = storage.directoryFor(QStringLiteral("telegram.tdlib"));
        QVERIFY(sessionDir.startsWith(root));
        QFile f(sessionDir + QStringLiteral("/td.binlog"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("session");
        f.close();

        // A neighbour outside the root must survive any request.
        const QString outside = dir.filePath(QStringLiteral("keep.txt"));
        QFile keep(outside);
        QVERIFY(keep.open(QIODevice::WriteOnly));
        keep.close();

        QVERIFY(storage.directoryFor(QStringLiteral("..")).isEmpty());
        QVERIFY(!storage.removeAll(QStringLiteral("..")));
        QVERIFY(!storage.removeAll(QString()));
        QVERIFY(storage.removeAll(QStringLiteral("telegram.tdlib")));
        QVERIFY(!QDir(sessionDir).exists());
        QVERIFY(QFile::exists(outside));
        QVERIFY(QDir(root).exists());
    }
};

QTEST_MAIN(TestShareSecurity)
#include "tst_sharesecurity.moc"
