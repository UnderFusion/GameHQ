#include "updates/VersionNumber.h"

#include <QTest>

// VersionNumber parses "major.minor.patch" (with an optional leading v) and
// compares versions numerically. Never compare versions as strings.
class TestVersionNumber : public QObject
{
    Q_OBJECT

private slots:
    void parse_valid_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<int>("major");
        QTest::addColumn<int>("minor");
        QTest::addColumn<int>("patch");

        QTest::newRow("plain") << "1.2.3" << 1 << 2 << 3;
        QTest::newRow("zeroes") << "0.0.0" << 0 << 0 << 0;
        QTest::newRow("lowercase v") << "v1.2.3" << 1 << 2 << 3;
        QTest::newRow("uppercase V") << "V1.2.3" << 1 << 2 << 3;
        QTest::newRow("multi-digit components") << "12.34.567" << 12 << 34 << 567;
        QTest::newRow("current app version") << "0.6.6" << 0 << 6 << 6;
    }

    void parse_valid()
    {
        QFETCH(QString, input);
        QFETCH(int, major);
        QFETCH(int, minor);
        QFETCH(int, patch);

        const auto version = VersionNumber::parse(input);
        QVERIFY(version.has_value());
        QCOMPARE(version->major(), major);
        QCOMPARE(version->minor(), minor);
        QCOMPARE(version->patch(), patch);
    }

    void parse_invalid_data()
    {
        QTest::addColumn<QString>("input");

        QTest::newRow("empty") << "";
        QTest::newRow("major-minor only") << "1.2";
        QTest::newRow("four components") << "1.2.3.4";
        QTest::newRow("leading zero major") << "01.2.3";
        QTest::newRow("leading zero minor") << "1.02.3";
        QTest::newRow("leading zero patch") << "1.2.03";
        QTest::newRow("prerelease suffix") << "1.2.3-beta";
        QTest::newRow("build metadata") << "1.2.3+build5";
        QTest::newRow("negative") << "1.2.-3";
        QTest::newRow("non-numeric") << "a.b.c";
        QTest::newRow("whitespace") << " 1.2.3";
        QTest::newRow("trailing whitespace") << "1.2.3 ";
        QTest::newRow("double v") << "vv1.2.3";
        QTest::newRow("just v") << "v";
        QTest::newRow("empty component") << "1..3";
    }

    void parse_invalid()
    {
        QFETCH(QString, input);
        QVERIFY(!VersionNumber::parse(input).has_value());
    }

    void ordering()
    {
        const auto v123 = VersionNumber::parse("1.2.3");
        const auto v124 = VersionNumber::parse("1.2.4");
        const auto v130 = VersionNumber::parse("1.3.0");
        const auto v200 = VersionNumber::parse("2.0.0");
        const auto v1_10_0 = VersionNumber::parse("1.10.0");
        const auto v1_9_99 = VersionNumber::parse("1.9.99");
        QVERIFY(v123.has_value() && v124.has_value() && v130.has_value()
                && v200.has_value() && v1_10_0.has_value() && v1_9_99.has_value());

        QVERIFY(*v123 < *v124);
        QVERIFY(*v124 < *v130);
        QVERIFY(*v130 < *v200);
        // Numeric, not lexical: 1.9.99 must sort below 1.10.0.
        QVERIFY(*v1_9_99 < *v1_10_0);
        QVERIFY(*v200 > *v123);
    }

    void equality_ignoresVPrefix()
    {
        const auto a = VersionNumber::parse("1.2.3");
        const auto b = VersionNumber::parse("v1.2.3");
        QVERIFY(a.has_value() && b.has_value());
        QCOMPARE(*a, *b);
    }

    void comparisonOperators_areConsistent()
    {
        const auto lower = VersionNumber::parse("1.2.3");
        const auto higher = VersionNumber::parse("1.2.4");
        QVERIFY(lower.has_value() && higher.has_value());

        QVERIFY(*lower <= *higher);
        QVERIFY(*higher >= *lower);
        QVERIFY(*lower != *higher);
        QVERIFY(*lower <= *lower);
        QVERIFY(*lower >= *lower);
    }

    void toString_roundTrips()
    {
        const auto version = VersionNumber::parse("v1.2.3");
        QVERIFY(version.has_value());
        QCOMPARE(version->toString(), QStringLiteral("1.2.3"));
    }

    void releaseIdentity_ranksBetasBelowTheirStableRelease()
    {
        const auto beta1 = ReleaseIdentity::parse(QStringLiteral("v0.7.10-beta1"));
        const auto beta2 = ReleaseIdentity::parse(QStringLiteral("0.7.10-beta2"));
        const auto beta10 = ReleaseIdentity::parse(QStringLiteral("0.7.10-beta10"));
        const auto stable = ReleaseIdentity::parse(QStringLiteral("0.7.10"));
        const auto next = ReleaseIdentity::parse(QStringLiteral("0.7.11-beta1"));
        QVERIFY(beta1 && beta2 && beta10 && stable && next);
        QVERIFY(*beta1 < *beta2);
        QVERIFY(*beta2 < *beta10);
        QVERIFY(*beta10 < *stable);
        QVERIFY(*stable < *next);
        QVERIFY(*ReleaseIdentity::parse(QStringLiteral("0.7.9")) < *beta1);
        QCOMPARE(beta2->toString(), QStringLiteral("0.7.10-beta2"));
        QCOMPARE(stable->toString(), QStringLiteral("0.7.10"));
        QVERIFY(!stable->isBeta());

        QVERIFY(!ReleaseIdentity::parse(QStringLiteral("0.7.10-beta0")));
        QVERIFY(!ReleaseIdentity::parse(QStringLiteral("0.7.10-beta01")));
        QVERIFY(!ReleaseIdentity::parse(QStringLiteral("0.7.10-rc1")));
        QVERIFY(!ReleaseIdentity::parse(QStringLiteral("0.7.10-beta")));
    }
};

QTEST_APPLESS_MAIN(TestVersionNumber)
#include "tst_versionnumber.moc"
