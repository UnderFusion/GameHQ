// Border-state derivation (p4-2). Pure logic, no capture device and no WinRT.
//
// This file locks down two mistakes.
//
// 1. Treating a successful put_IsBorderRequired(false) as proof that the yellow
//    Windows capture border is gone. Windows ignores the flag when the caller has no
//    Borderless access but still returns S_OK, so "Hidden" must require the access
//    grant too — and must be unreachable on Windows 10.
// 2. Mis-decoding AppCapabilityAccessStatus. The first cut of this code assumed
//    Allowed=0; the real metadata says Allowed=4 and DeniedBySystem=0. With the
//    reversed mapping a hard system denial would have decoded as "Allowed" and
//    produced a false Hidden, so the raw integers are pinned here as well as by the
//    static_asserts in the header.

#include "capture/CaptureBorderState.h"

#include <QtTest>

using namespace CaptureBorder;

namespace {

// Everything green: Windows 11, interface present, access granted, flag stuck.
Facts allowedFacts()
{
    Facts f;
    f.osBuild = 26200;
    f.sessionInterfaceAvailable = true;
    f.accessStatus = AccessAllowed;
    f.setterSucceeded = true;
    f.readBackSucceeded = true;
    f.readBackBorderRequired = false;
    return f;
}

// What this unpackaged build actually measured in p4-1 on Windows 11 build 26200:
// the access request came back Allowed (raw 4) and the flag read back as suppressed.
Facts measuredFacts()
{
    Facts f = allowedFacts();
    f.accessStatus = 4;          // deliberately the raw integer, not the named constant
    return f;
}

} // namespace

class TestCaptureBorderState : public QObject
{
    Q_OBJECT
private slots:
    void hiddenNeedsEveryCondition();
    void windows10IsNeverHidden();
    void windows10IsNeverHidden_data();
    void successfulSetterAloneIsNotHidden();
    void successfulSetterAloneIsNotHidden_data();
    void missingInterfaceIsUnsupported();
    void unreadableBuildIsUnknown();
    void deniedAccessStatuses();
    void deniedAccessStatuses_data();
    void readBackContradictionIsDenied();
    void rawAccessIntegersMatchWindowsMetadata();
    void rawStatusFourIsAllowedAndCanBeHidden();
    void rawStatusThreeIsPromptAndNeverHidden();
    void rawStatusZeroIsSystemDenialNotAllowed();
    void failedCallsAreUnknownNotHidden();
    void failedCallsAreUnknownNotHidden_data();
    void describeMentionsStateAndAccess();
};

void TestCaptureBorderState::hiddenNeedsEveryCondition()
{
    QCOMPARE(derive(allowedFacts()), Hidden);
}

void TestCaptureBorderState::windows10IsNeverHidden_data()
{
    QTest::addColumn<unsigned long>("build");
    QTest::newRow("windows 10 1809")   << 17763ul;
    QTest::newRow("windows 10 21h2")   << 19044ul;
    QTest::newRow("one below the gate") << (kFirstBuildWithBorderControl - 1);
}

void TestCaptureBorderState::windows10IsNeverHidden()
{
    QFETCH(unsigned long, build);
    // Even with a (hypothetically) present interface and a granted request, a
    // pre-22000 build must report Unsupported and never Hidden.
    Facts f = allowedFacts();
    f.osBuild = build;
    QCOMPARE(derive(f), Unsupported);
}

void TestCaptureBorderState::successfulSetterAloneIsNotHidden_data()
{
    QTest::addColumn<int>("access");
    QTest::newRow("never asked")           << int(AccessNotRequested);
    QTest::newRow("user prompt required")  << int(AccessUserPromptRequired);
    QTest::newRow("denied by system")      << int(AccessDeniedBySystem);
    QTest::newRow("denied by user")        << int(AccessDeniedByUser);
    QTest::newRow("not declared by app")   << int(AccessNotDeclaredByApp);
}

void TestCaptureBorderState::successfulSetterAloneIsNotHidden()
{
    QFETCH(int, access);
    Facts f = allowedFacts();          // setter ok, read-back says suppressed
    f.accessStatus = access;
    QVERIFY(derive(f) != Hidden);
}

void TestCaptureBorderState::missingInterfaceIsUnsupported()
{
    Facts f = allowedFacts();
    f.sessionInterfaceAvailable = false;
    QCOMPARE(derive(f), Unsupported);
}

void TestCaptureBorderState::unreadableBuildIsUnknown()
{
    Facts f = allowedFacts();
    f.osBuild = 0;
    QCOMPARE(derive(f), Unknown);
}

void TestCaptureBorderState::deniedAccessStatuses_data()
{
    QTest::addColumn<int>("access");
    QTest::newRow("user prompt required") << int(AccessUserPromptRequired);
    QTest::newRow("denied by system")     << int(AccessDeniedBySystem);
    QTest::newRow("denied by user")       << int(AccessDeniedByUser);
    QTest::newRow("not declared by app")  << int(AccessNotDeclaredByApp);
}

void TestCaptureBorderState::deniedAccessStatuses()
{
    QFETCH(int, access);
    Facts f = allowedFacts();
    f.accessStatus = access;
    QCOMPARE(derive(f), Denied);

    // Never asked is different from asked-and-refused: that one is Unknown.
    f.accessStatus = AccessNotRequested;
    QCOMPARE(derive(f), Unknown);
}

void TestCaptureBorderState::readBackContradictionIsDenied()
{
    // Access granted and the put succeeded, but the OS still reports the border as
    // required — believe the read-back, not the HRESULT.
    Facts f = allowedFacts();
    f.readBackBorderRequired = true;
    QCOMPARE(derive(f), Denied);
}

void TestCaptureBorderState::failedCallsAreUnknownNotHidden_data()
{
    QTest::addColumn<bool>("setter");
    QTest::addColumn<bool>("readBack");
    QTest::newRow("setter failed")     << false << true;
    QTest::newRow("read-back failed")  << true  << false;
    QTest::newRow("both failed")       << false << false;
}

void TestCaptureBorderState::failedCallsAreUnknownNotHidden()
{
    QFETCH(bool, setter);
    QFETCH(bool, readBack);
    Facts f = allowedFacts();
    f.setterSucceeded = setter;
    f.readBackSucceeded = readBack;
    QCOMPARE(derive(f), Unknown);
}

// The exact integral contract with Windows. Read out of Windows.Security.winmd by
// tools/spikes/winrt_enum_probe.exe; if Microsoft ever renumbers it, this fails loudly
// instead of silently turning denials into grants.
void TestCaptureBorderState::rawAccessIntegersMatchWindowsMetadata()
{
    QCOMPARE(int(AccessDeniedBySystem),     0);
    QCOMPARE(int(AccessNotDeclaredByApp),   1);
    QCOMPARE(int(AccessDeniedByUser),       2);
    QCOMPARE(int(AccessUserPromptRequired), 3);
    QCOMPARE(int(AccessAllowed),            4);
    QVERIFY(int(AccessNotRequested) < 0);

    // Names must follow the numbers, or the log line lies about what happened.
    QCOMPARE(accessStatusName(0), QStringLiteral("DeniedBySystem"));
    QCOMPARE(accessStatusName(3), QStringLiteral("UserPromptRequired"));
    QCOMPARE(accessStatusName(4), QStringLiteral("Allowed"));
}

// Raw 4 is a grant, so derivation must carry on to the setter/read-back and may reach
// Hidden — this is the case the reversed mapping used to turn into a false Denied.
void TestCaptureBorderState::rawStatusFourIsAllowedAndCanBeHidden()
{
    Facts f = measuredFacts();
    QCOMPARE(f.accessStatus, int(AccessAllowed));
    QCOMPARE(derive(f), Hidden);

    // Same grant, but the OS still insists: the read-back wins over the grant.
    f.readBackBorderRequired = true;
    QCOMPARE(derive(f), Denied);
}

// Raw 3 is UserPromptRequired: a real grant never happened, so Hidden is off the table
// no matter how happily the setter returned S_OK.
void TestCaptureBorderState::rawStatusThreeIsPromptAndNeverHidden()
{
    Facts f = allowedFacts();
    f.accessStatus = 3;
    QCOMPARE(f.accessStatus, int(AccessUserPromptRequired));
    QCOMPARE(derive(f), Denied);
    QVERIFY(derive(f) != Hidden);
}

// Raw 0 is the hard system denial. Under the old reversed mapping this decoded as
// "Allowed" and produced Hidden — the exact regression this test exists to prevent.
void TestCaptureBorderState::rawStatusZeroIsSystemDenialNotAllowed()
{
    Facts f = allowedFacts();
    f.accessStatus = 0;
    QCOMPARE(f.accessStatus, int(AccessDeniedBySystem));
    QVERIFY2(derive(f) != Hidden, "a system denial must never be reported as a hidden border");
    QCOMPARE(derive(f), Denied);
}

void TestCaptureBorderState::describeMentionsStateAndAccess()
{
    // A denied case: the detail must name the real status and must not read as success.
    Facts denied = allowedFacts();
    denied.accessStatus = AccessDeniedByUser;
    const State deniedState = derive(denied);
    QCOMPARE(deniedState, Denied);

    const QString text = describe(denied, deniedState);
    QVERIFY2(text.contains(QStringLiteral("denied")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("DeniedByUser")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("26200")), qPrintable(text));
    QVERIFY2(!text.startsWith(QStringLiteral("hidden")), qPrintable(text));

    // The measured case reports Allowed, and says so.
    const Facts measured = measuredFacts();
    const QString ok = describe(measured, derive(measured));
    QVERIFY2(ok.startsWith(QStringLiteral("hidden")), qPrintable(ok));
    QVERIFY2(ok.contains(QStringLiteral("Allowed")), qPrintable(ok));
}

QTEST_APPLESS_MAIN(TestCaptureBorderState)
#include "tst_captureborderstate.moc"
