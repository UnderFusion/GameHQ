#include <QtTest>

#include <QHash>
#include <QSet>

#include "input/PnpDeviceApi.h"

// cpo-c03b investigation slice: the PnP/CfgMgr32 seam is the only production
// unit this test covers. It cannot reach real hardware (that is what
// tools/spikes/pnp_container_probe.cpp is for), so it pins the two things the
// seam itself must guarantee:
//
//   * canonical comparison forms — two APIs describing one container must
//     produce byte-identical text;
//   * fail-closed chaining — an unanswerable step yields `queried == false`
//     and never a guess, never a stale value, never a second query.
namespace
{
const QString kPathDualSense = QStringLiteral("\\\\?\\hid#vid_054c&pid_0ce6#7&1a2b3c&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}");
const QString kPathDualSenseSecond = QStringLiteral("\\\\?\\hid#vid_054c&pid_0ce6#7&1a2b3c&0&0001#{4d1e55b2-f16f-11cf-88cb-001111000030}");
const QString kPathGameSir = QStringLiteral("\\\\?\\hid#vid_3537&pid_1004#6&9f8e7d&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}");
const QString kInstanceDualSense = QStringLiteral("HID\\VID_054C&PID_0CE6\\7&1A2B3C&0&0000");
const QString kInstanceDualSenseSecond = QStringLiteral("HID\\VID_054C&PID_0CE6\\7&1A2B3C&0&0001");
const QString kInstanceGameSir = QStringLiteral("HID\\VID_3537&PID_1004\\6&9F8E7D&0&0000");
const QString kContainerDualSense = QStringLiteral("{14f0cd9e-0f5a-4a5f-9d3f-2b1c6a7e8d90}");
const QString kContainerGameSir = QStringLiteral("{8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c}");
// Memory-order bytes of kContainerGameSir, exactly as `bytesId` hex-encodes a
// GUID that WinRT/GameInput handed out.
const QString kContainerGameSirHex = QStringLiteral("06d27e8c8a3f2748b3abae9e1faefc6c");

// Stands in for CfgMgr32. It mirrors the documented contract of a real
// implementation: it answers only about devices currently in its tables and
// refuses (queried = false) for everything else.
class FakePnpDeviceApi : public PnpDeviceApi
{
public:
    QHash<QString, QString> instanceByPath;
    QHash<QString, QString> containerByInstance;
    QSet<QString> absentInstances;
    int instanceQueries = 0;
    int containerQueries = 0;

    InstanceId deviceInstanceIdForInterfacePath(const QString& interfacePath) override
    {
        ++instanceQueries;
        const auto found = instanceByPath.constFind(normalizeInterfacePath(interfacePath));
        if (found == instanceByPath.cend())
            return {};
        return { true, found.value() };
    }

    Container containerIdForDeviceInstance(const QString& deviceInstanceId) override
    {
        ++containerQueries;
        const QString instance = normalizeDeviceInstanceId(deviceInstanceId);
        if (absentInstances.contains(instance))
            return {};
        const auto found = containerByInstance.constFind(instance);
        if (found == containerByInstance.cend())
            return {};
        return { true, found.value() };
    }
};

// An implementation that answers with the sloppiest spellings the seam contract
// still has to survive: padded, lower-case instance IDs and a container string
// that is not a GUID at all.
class SloppyPnpDeviceApi : public PnpDeviceApi
{
public:
    QString lastInstanceReceived;
    QString containerAnswer = QStringLiteral("{14F0CD9E-0F5A-4A5F-9D3F-2B1C6A7E8D90}");

    InstanceId deviceInstanceIdForInterfacePath(const QString&) override
    {
        return { true, QStringLiteral("  hid\\vid_054c&pid_0ce6\\7&1a2b3c&0&0000  ") };
    }

    Container containerIdForDeviceInstance(const QString& deviceInstanceId) override
    {
        lastInstanceReceived = deviceInstanceId;
        return { true, containerAnswer };
    }
};
} // namespace

class PnpDeviceApiTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizesContainerSpellings()
    {
        const QString canonical = QStringLiteral("{14f0cd9e-0f5a-4a5f-9d3f-2b1c6a7e8d90}");
        QCOMPARE(PnpDeviceApi::normalizeContainerId(canonical), canonical);
        QCOMPARE(PnpDeviceApi::normalizeContainerId(QStringLiteral("{14F0CD9E-0F5A-4A5F-9D3F-2B1C6A7E8D90}")),
                 canonical);
        QCOMPARE(PnpDeviceApi::normalizeContainerId(QStringLiteral("14f0cd9e-0f5a-4a5f-9d3f-2b1c6a7e8d90")),
                 canonical);
        QCOMPARE(PnpDeviceApi::normalizeContainerId(QStringLiteral("  {14f0cd9e-0f5a-4a5f-9d3f-2b1c6a7e8d90}  ")),
                 canonical);
    }

    void rejectsMalformedContainers()
    {
        // Anything that is not a GUID must produce an empty comparison key, so
        // a malformed value can never compare equal to a real container.
        QCOMPARE(PnpDeviceApi::normalizeContainerId(QString()), QString());
        QCOMPARE(PnpDeviceApi::normalizeContainerId(QStringLiteral("not-a-guid")), QString());
        QCOMPARE(PnpDeviceApi::normalizeContainerId(QStringLiteral("14f0cd9e-0f5a-4a5f-9d3f")), QString());
        QCOMPARE(PnpDeviceApi::normalizeContainerId(QStringLiteral("14f0cd9e-0f5a-4a5f-9d3f-2b1c6a7e8d9g")),
                 QString());
        QCOMPARE(PnpDeviceApi::normalizeContainerId(QStringLiteral("{14f0cd9e-0f5a-4a5f-9d3f-2b1c6a7e8d90-extra}")),
                 QString());
    }

    void normalizesDeviceInstanceIds()
    {
        QCOMPARE(PnpDeviceApi::normalizeDeviceInstanceId(QStringLiteral("hid\\vid_054c&pid_0ce6\\7&1a2b3c&0&0000")),
                 kInstanceDualSense);
        QCOMPARE(PnpDeviceApi::normalizeDeviceInstanceId(QStringLiteral("  HID\\VID_054C&PID_0CE6\\7&1A2B3C&0&0000 \n")),
                 kInstanceDualSense);
        QCOMPARE(PnpDeviceApi::normalizeDeviceInstanceId(QString()), QString());
    }

    void normalizesInterfacePaths()
    {
        QCOMPARE(PnpDeviceApi::normalizeInterfacePath(kPathDualSense.toUpper()), kPathDualSense);
        QCOMPARE(PnpDeviceApi::normalizeInterfacePath(QStringLiteral("  ") + kPathDualSense), kPathDualSense);
        QCOMPARE(PnpDeviceApi::normalizeInterfacePath(QString()), QString());
    }

    void convertsRawContainerBytesToGuidText()
    {
        QCOMPARE(PnpDeviceApi::containerFromRawBytesHex(kContainerGameSirHex), kContainerGameSir);
        QCOMPARE(PnpDeviceApi::containerFromRawBytesHex(kContainerGameSirHex.toUpper()), kContainerGameSir);
        // Wrong lengths, separators and non-hex characters are refusals, not
        // approximations: a half-read container must not match a real device.
        QCOMPARE(PnpDeviceApi::containerFromRawBytesHex(QStringLiteral("06d27e8c")), QString());
        QCOMPARE(PnpDeviceApi::containerFromRawBytesHex(kContainerGameSir), QString());
        QCOMPARE(PnpDeviceApi::containerFromRawBytesHex(kContainerGameSirHex.left(31) + QStringLiteral("z")),
                 QString());
        QCOMPARE(PnpDeviceApi::containerFromRawBytesHex(QString()), QString());
    }

    void containerFromRawBytesMatchesGuidTextRoundTrip()
    {
        // The same physical device observed once through CfgMgr32 ({guid} text)
        // and once through GameInput (raw bytes in memory order, hex-encoded the
        // way `bytesId` stores them) must compare equal.
        const QString fromGameInput =
            PnpDeviceApi::containerFromRawBytesHex(QStringLiteral("9ecdf0145a0f5f4a9d3f2b1c6a7e8d90"));

        FakePnpDeviceApi api;
        api.instanceByPath.insert(kPathDualSense, kInstanceDualSense);
        api.containerByInstance.insert(kInstanceDualSense, kContainerDualSense);
        const PnpDeviceApi::Container fromPnp = api.containerIdForInterfacePath(kPathDualSense);

        QVERIFY(fromPnp.queried);
        QCOMPARE(fromGameInput, fromPnp.value);
    }

    void chainedQueryReturnsCanonicalContainer()
    {
        FakePnpDeviceApi api;
        api.instanceByPath.insert(kPathDualSense, kInstanceDualSense);
        api.containerByInstance.insert(kInstanceDualSense, kContainerDualSense);

        const PnpDeviceApi::Container answer = api.containerIdForInterfacePath(kPathDualSense);
        QVERIFY(answer.queried);
        QCOMPARE(answer.value, kContainerDualSense);
        QCOMPARE(api.instanceQueries, 1);
        QCOMPARE(api.containerQueries, 1);
    }

    void refusesWhenInterfacePathIsUnknown()
    {
        FakePnpDeviceApi api;
        api.containerByInstance.insert(kInstanceDualSense, kContainerDualSense);

        const PnpDeviceApi::Container answer = api.containerIdForInterfacePath(kPathDualSense);
        QVERIFY(!answer.queried);
        QVERIFY(answer.value.isEmpty());
        // Fail-closed also means: no second query, no inference from the path.
        QCOMPARE(api.containerQueries, 0);
    }

    void refusesWhenDeviceIsAbsentOrCloaked()
    {
        FakePnpDeviceApi api;
        api.instanceByPath.insert(kPathDualSense, kInstanceDualSense);
        api.containerByInstance.insert(kInstanceDualSense, kContainerDualSense);
        api.absentInstances.insert(kInstanceDualSense);

        const PnpDeviceApi::Container answer = api.containerIdForInterfacePath(kPathDualSense);
        QVERIFY(!answer.queried);
        QVERIFY(answer.value.isEmpty());
    }

    void refusesMalformedContainerFromTheOs()
    {
        SloppyPnpDeviceApi api;
        api.containerAnswer = QStringLiteral("container-17");  // property came back unusable
        const PnpDeviceApi::Container answer = api.containerIdForInterfacePath(kPathGameSir);
        QVERIFY(!answer.queried);
        QVERIFY(answer.value.isEmpty());
    }

    void canonicalizesWhatTheOsPrints()
    {
        SloppyPnpDeviceApi api;
        const PnpDeviceApi::Container answer = api.containerIdForInterfacePath(kPathDualSense);
        // The instance the second step received was already canonical...
        QCOMPARE(api.lastInstanceReceived, kInstanceDualSense);
        // ...and so is the container the caller sees, whatever spelling arrived.
        QVERIFY(answer.queried);
        QCOMPARE(answer.value, QStringLiteral("{14f0cd9e-0f5a-4a5f-9d3f-2b1c6a7e8d90}"));
    }

    void keepsTwoIdenticalPadsApart()
    {
        FakePnpDeviceApi api;
        api.instanceByPath.insert(kPathDualSense, kInstanceDualSense);
        api.instanceByPath.insert(kPathDualSenseSecond, kInstanceDualSenseSecond);
        api.containerByInstance.insert(kInstanceDualSense, kContainerDualSense);
        api.containerByInstance.insert(kInstanceDualSenseSecond,
                                       QStringLiteral("{0a1b2c3d-4e5f-6071-8293-a4b5c6d7e8f9}"));

        const auto first = api.containerIdForInterfacePath(kPathDualSense);
        const auto second = api.containerIdForInterfacePath(kPathDualSenseSecond);
        QVERIFY(first.queried && second.queried);
        QVERIFY(first.value != second.value);
    }

    void reportsSharedContainerForSharedTopology()
    {
        // Two HID collections of one physical pad: the seam must report the one
        // container Windows assigned, and must NOT invent a disambiguator.
        FakePnpDeviceApi api;
        api.instanceByPath.insert(kPathDualSense, kInstanceDualSense);
        api.instanceByPath.insert(kPathDualSenseSecond, kInstanceDualSenseSecond);
        api.containerByInstance.insert(kInstanceDualSense, kContainerDualSense);
        api.containerByInstance.insert(kInstanceDualSenseSecond, kContainerDualSense);

        const auto first = api.containerIdForInterfacePath(kPathDualSense);
        const auto second = api.containerIdForInterfacePath(kPathDualSenseSecond);
        QVERIFY(first.queried && second.queried);
        QCOMPARE(first.value, second.value);
    }

    void isOrderIndependent()
    {
        FakePnpDeviceApi forward;
        FakePnpDeviceApi backward;
        const auto setup = [](FakePnpDeviceApi* api) {
            api->instanceByPath.insert(kPathDualSense, kInstanceDualSense);
            api->instanceByPath.insert(kPathGameSir, kInstanceGameSir);
            api->containerByInstance.insert(kInstanceDualSense, kContainerDualSense);
            api->containerByInstance.insert(kInstanceGameSir, kContainerGameSir);
        };
        setup(&forward);
        setup(&backward);

        const auto forwardSony = forward.containerIdForInterfacePath(kPathDualSense);
        const auto forwardGameSir = forward.containerIdForInterfacePath(kPathGameSir);
        const auto backwardGameSir = backward.containerIdForInterfacePath(kPathGameSir);
        const auto backwardSony = backward.containerIdForInterfacePath(kPathDualSense);

        QCOMPARE(forwardSony.value, backwardSony.value);
        QCOMPARE(forwardGameSir.value, backwardGameSir.value);
        QVERIFY(forwardSony.value != forwardGameSir.value);
    }

    void answersFreshAfterReconnect()
    {
        FakePnpDeviceApi api;
        api.instanceByPath.insert(kPathDualSense, kInstanceDualSense);
        api.containerByInstance.insert(kInstanceDualSense, kContainerDualSense);

        QVERIFY(api.containerIdForInterfacePath(kPathDualSense).queried);
        api.absentInstances.insert(kInstanceDualSense);  // unplugged
        QVERIFY(!api.containerIdForInterfacePath(kPathDualSense).queried);
        api.absentInstances.remove(kInstanceDualSense);  // replugged
        const auto again = api.containerIdForInterfacePath(kPathDualSense);
        QVERIFY(again.queried);
        QCOMPARE(again.value, kContainerDualSense);
        // Three arrivals, three interface queries, three container queries: the
        // seam re-asks the OS every time and never served a cached answer for
        // an earlier device (the middle arrival gets a refusal, not a value).
        QCOMPARE(api.instanceQueries, 3);
        QCOMPARE(api.containerQueries, 3);
    }
};

QTEST_GUILESS_MAIN(PnpDeviceApiTest)

#include "tst_pnpdeviceapi.moc"
