#include <QtTest>

#include "integration/IntegrationClient.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>

class IntegrationClientTest : public QObject
{
    Q_OBJECT
private slots:
    void forwardsActivation_data();
    void forwardsActivation();
    void unavailableServerIsBounded();
};

namespace
{
// A real GameHQ.exe instance already listening on the production pipe name
// would otherwise make these tests bind (or connect) to the wrong server.
QString testServerName()
{
    return QStringLiteral("GameHQ.Local.Test.%1").arg(QCoreApplication::applicationPid());
}
}

void IntegrationClientTest::forwardsActivation_data()
{
    QTest::addColumn<QString>("argument");
    QTest::newRow("show") << QStringLiteral("--show");
    QTest::newRow("gallery") << QStringLiteral("--open-gallery");
}

void IntegrationClientTest::forwardsActivation()
{
    QFETCH(QString, argument);
    const QString name = testServerName();
    QProcess fixture;
    fixture.setProgram(QStringLiteral(INTEGRATION_SERVER_FIXTURE));
    fixture.setArguments({ name });
    fixture.start();
    QVERIFY(fixture.waitForStarted(1000));
    // Output can arrive in chunks; wait for the complete readiness line.
    QTRY_VERIFY_WITH_TIMEOUT(fixture.canReadLine(), 1000);
    QCOMPARE(fixture.readLine().trimmed(), QByteArray("READY"));

    QString error;
    QVERIFY2(IntegrationClient::forwardSecondInstance(
                 { QStringLiteral("GameHQ.exe"), argument },
                 QStringLiteral("1.2.3"), error, name), qPrintable(error));
    QVERIFY(fixture.waitForFinished(1000));
    QCOMPARE(fixture.exitCode(), 0);
}

void IntegrationClientTest::unavailableServerIsBounded()
{
    QElapsedTimer timer;
    timer.start();
    QString error;
    QVERIFY(!IntegrationClient::forwardSecondInstance(
        { QStringLiteral("GameHQ.exe") }, QStringLiteral("1.2.3"), error, testServerName()));
    QVERIFY(!error.isEmpty());
    QVERIFY(timer.elapsed() < 1000);
}

QTEST_GUILESS_MAIN(IntegrationClientTest)
#include "tst_integrationclient.moc"
