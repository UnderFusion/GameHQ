#include "input/PhysicalControllerRegistry.h"

#include <QtTest>

#include <algorithm>

using namespace ModernInput;

class PhysicalControllerRegistryTest : public QObject
{
    Q_OBJECT

private slots:
    void strongIdentityMergesCapabilities()
    {
        PhysicalControllerRegistry registry;
        ProviderObservation sony{ControllerProvider::SonyRaw, QStringLiteral("raw-1")};
        sony.containerId = QStringLiteral("container-a");
        sony.capabilities = ControllerCapability::StandardControls;
        ProviderObservation gameInput{ControllerProvider::GameInput, QStringLiteral("gi-1")};
        gameInput.containerId = sony.containerId;
        gameInput.capabilities = ControllerCapability::SystemShare
            | ControllerCapability::Guide | ControllerCapability::ExtraControls;

        const QString first = registry.observe(sony);
        const QString second = registry.observe(gameInput);

        QCOMPARE(first, second);
        const auto* logical = registry.controller(first);
        QVERIFY(logical);
        QCOMPARE(logical->providers.size(), 2);
        QVERIFY(logical->capabilities().testFlag(ControllerCapability::SystemShare));
        QCOMPARE(registry.preferredProvider(first, ControllerCapability::StandardControls),
                 ControllerProvider::SonyRaw);
        QCOMPARE(registry.preferredProvider(first, ControllerCapability::SystemShare),
                 ControllerProvider::GameInput);
    }

    void weakVidPidNeverMergesIdenticalModels()
    {
        PhysicalControllerRegistry registry;
        ProviderObservation first{ControllerProvider::XInput, QStringLiteral("slot-0")};
        first.vendorId = 0x1234;
        first.productId = 0x5678;
        ProviderObservation second = first;
        second.providerDeviceId = QStringLiteral("slot-1");

        const QString firstId = registry.observe(first);
        const QString secondId = registry.observe(second);

        QVERIFY(firstId != secondId);
        QCOMPARE(registry.controllers().size(), 2);
        QCOMPARE(registry.controller(firstId)->confidence, IdentityConfidence::Weak);
    }

    void correlatedIdentityRejectsAmbiguousTopology()
    {
        PhysicalControllerRegistry registry;
        ProviderObservation first{ControllerProvider::XInput, QStringLiteral("x-1")};
        first.topologyRoot = QStringLiteral("receiver-root");
        first.containerId = QStringLiteral("container-1");
        ProviderObservation second{ControllerProvider::XInput, QStringLiteral("x-2")};
        second.topologyRoot = first.topologyRoot;
        second.containerId = QStringLiteral("container-2");
        const QString firstId = registry.observe(first);
        const QString secondId = registry.observe(second);
        QVERIFY(firstId != secondId);

        ProviderObservation gameInput{ControllerProvider::GameInput, QStringLiteral("gi")};
        gameInput.topologyRoot = first.topologyRoot;
        const QString thirdId = registry.observe(gameInput);
        QVERIFY(thirdId != firstId);
        QVERIFY(thirdId != secondId);
    }

    void reversedObservationOrderYieldsSameStrongLogicalId()
    {
        // The logical ID is persisted (binding profiles, controller_layouts):
        // it must not depend on which provider observed the device first.
        ProviderObservation sony{ControllerProvider::SonyRaw, QStringLiteral("raw-1")};
        sony.appLocalDeviceId = QStringLiteral("app-local-1");
        ProviderObservation gameInput{ControllerProvider::GameInput, QStringLiteral("gi-1")};
        gameInput.appLocalDeviceId = sony.appLocalDeviceId;

        PhysicalControllerRegistry first;
        const QString firstId = first.observe(sony);
        QCOMPARE(first.observe(gameInput), firstId);

        PhysicalControllerRegistry second;
        const QString secondId = second.observe(gameInput);
        QCOMPARE(second.observe(sony), secondId);

        QCOMPARE(firstId, secondId);
    }

    void reversedOrderKeepsTwoDevicesDistinctAndStable()
    {
        ProviderObservation padA{ControllerProvider::GameInput, QStringLiteral("gi-a")};
        padA.appLocalDeviceId = QStringLiteral("app-a");
        ProviderObservation padB{ControllerProvider::GameInput, QStringLiteral("gi-b")};
        padB.appLocalDeviceId = QStringLiteral("app-b");

        PhysicalControllerRegistry first;
        const QString aFirst = first.observe(padA);
        const QString bFirst = first.observe(padB);
        PhysicalControllerRegistry second;
        const QString bSecond = second.observe(padB);
        const QString aSecond = second.observe(padA);

        QVERIFY(aFirst != bFirst);
        QCOMPARE(aFirst, aSecond);
        QCOMPARE(bFirst, bSecond);
    }

    void conflictingStrongIdsInSameContainerNeverMerge()
    {
        // A hub/receiver container can hold several endpoints: matching
        // containers must never override conflicting strong device IDs.
        PhysicalControllerRegistry registry;
        ProviderObservation one{ControllerProvider::GameInput, QStringLiteral("gi-1")};
        one.appLocalDeviceId = QStringLiteral("app-1");
        one.containerId = QStringLiteral("shared-container");
        ProviderObservation two{ControllerProvider::GameInput, QStringLiteral("gi-2")};
        two.appLocalDeviceId = QStringLiteral("app-2");
        two.containerId = one.containerId;

        const QString firstId = registry.observe(one);
        const QString secondId = registry.observe(two);
        QVERIFY(firstId != secondId);
        QCOMPARE(registry.controllers().size(), 2);
    }

    void exactAppLocalIdBeatsSharedRootEvidence()
    {
        // A shared root is topology evidence several endpoints can carry; an
        // app-local device ID names exactly one device. The exact identity
        // must win, never the root that happens to be scanned first.
        PhysicalControllerRegistry registry;
        ProviderObservation rooted{ControllerProvider::GameInput, QStringLiteral("gi-1")};
        rooted.topologyRoot = QStringLiteral("root-r");
        rooted.appLocalDeviceId = QStringLiteral("app-a");
        ProviderObservation exact{ControllerProvider::XInput, QStringLiteral("x-1")};
        exact.appLocalDeviceId = QStringLiteral("app-b");

        const QString rootedId = registry.observe(rooted);
        const QString exactId = registry.observe(exact);
        QVERIFY(rootedId != exactId);

        ProviderObservation legacy{ControllerProvider::WinMM, QStringLiteral("mm-1")};
        legacy.topologyRoot = rooted.topologyRoot;
        legacy.appLocalDeviceId = exact.appLocalDeviceId;
        QCOMPARE(registry.observe(legacy), exactId);
        QCOMPARE(registry.matchEvidence(ControllerProvider::WinMM, QStringLiteral("mm-1")),
                 MatchEvidence::AppLocalDeviceId);
    }

    void exactEndpointBeatsSharedContainerEvidence()
    {
        PhysicalControllerRegistry registry;
        ProviderObservation shared{ControllerProvider::GameInput, QStringLiteral("gi-1")};
        shared.containerId = QStringLiteral("container-c");
        ProviderObservation endpoint{ControllerProvider::XInput, QStringLiteral("x-1")};
        endpoint.endpointId = QStringLiteral("endpoint-e");

        const QString sharedId = registry.observe(shared);
        const QString endpointId = registry.observe(endpoint);
        QVERIFY(sharedId != endpointId);

        ProviderObservation legacy{ControllerProvider::WinMM, QStringLiteral("mm-1")};
        legacy.containerId = shared.containerId;
        legacy.endpointId = endpoint.endpointId;
        QCOMPARE(registry.observe(legacy), endpointId);
        QCOMPARE(registry.matchEvidence(ControllerProvider::WinMM, QStringLiteral("mm-1")),
                 MatchEvidence::EndpointId);
    }

    void ambiguousContainerNeverMerges()
    {
        // A hub container holding two distinct endpoints is ambiguous: an
        // observation carrying only that container must not pick one of them.
        PhysicalControllerRegistry registry;
        ProviderObservation one{ControllerProvider::XInput, QStringLiteral("x-1")};
        one.containerId = QStringLiteral("hub-container");
        one.endpointId = QStringLiteral("endpoint-1");
        ProviderObservation two{ControllerProvider::WinMM, QStringLiteral("mm-1")};
        two.containerId = one.containerId;
        two.endpointId = QStringLiteral("endpoint-2");
        const QString firstId = registry.observe(one);
        const QString secondId = registry.observe(two);
        QVERIFY(firstId != secondId);

        ProviderObservation third{ControllerProvider::GameInput, QStringLiteral("gi-1")};
        third.containerId = one.containerId;
        const QString thirdId = registry.observe(third);
        QVERIFY(thirdId != firstId);
        QVERIFY(thirdId != secondId);
        QCOMPARE(registry.controllers().size(), 3);
    }

    void twoPadsOnOneRootFromOneProviderStayDistinct()
    {
        // One provider reports each physical pad separately. Two pads behind
        // a single receiver root must never collapse onto one identity, not
        // through matching and not through a colliding deterministic ID.
        PhysicalControllerRegistry registry;
        ProviderObservation padA{ControllerProvider::XInput, QStringLiteral("x-1")};
        padA.topologyRoot = QStringLiteral("receiver-root");
        ProviderObservation padB{ControllerProvider::XInput, QStringLiteral("x-2")};
        padB.topologyRoot = padA.topologyRoot;

        const QString aId = registry.observe(padA);
        const QString bId = registry.observe(padB);
        QVERIFY(aId != bId);
        QCOMPARE(registry.controllers().size(), 2);
        QCOMPARE(registry.controller(aId)->providers.size(), 1);
        QCOMPARE(registry.controller(bId)->providers.size(), 1);
    }

    void contradictoryEndpointsBlockRootMatch()
    {
        PhysicalControllerRegistry registry;
        ProviderObservation first{ControllerProvider::GameInput, QStringLiteral("gi-1")};
        first.topologyRoot = QStringLiteral("root-r");
        first.endpointId = QStringLiteral("endpoint-1");
        ProviderObservation second{ControllerProvider::XInput, QStringLiteral("x-1")};
        second.topologyRoot = first.topologyRoot;
        second.endpointId = QStringLiteral("endpoint-2");

        const QString firstId = registry.observe(first);
        const QString secondId = registry.observe(second);
        QVERIFY(firstId != secondId);
        QCOMPARE(registry.controllers().size(), 2);
    }

    void twoIdenticalPadsWithStrongEndpointsStayDistinct()
    {
        PhysicalControllerRegistry registry;
        ProviderObservation padA{ControllerProvider::GameInput, QStringLiteral("gi-a")};
        padA.endpointId = QStringLiteral("endpoint-a");
        padA.modelFingerprint = QStringLiteral("054c:0ce6");
        padA.vendorId = 0x054c;
        padA.productId = 0x0ce6;
        ProviderObservation padB = padA;
        padB.providerDeviceId = QStringLiteral("gi-b");
        padB.endpointId = QStringLiteral("endpoint-b");

        const QString aId = registry.observe(padA);
        const QString bId = registry.observe(padB);
        QVERIFY(aId != bId);
        QCOMPARE(registry.controllers().size(), 2);
    }

    void matchingIsIndependentOfObservationOrder()
    {
        // m_controllers is a QHash: iteration order is unspecified and
        // seed-dependent, so the same population must resolve identically
        // whatever order it was built in.
        const auto populate = [](PhysicalControllerRegistry& registry, bool reversed) {
            QVector<ProviderObservation> pads;
            for (int index = 0; index < 6; ++index) {
                ProviderObservation pad{ControllerProvider::GameInput,
                                        QStringLiteral("gi-%1").arg(index)};
                pad.containerId = QStringLiteral("container-%1").arg(index);
                pad.topologyRoot = QStringLiteral("root-%1").arg(index);
                pads.push_back(pad);
            }
            if (reversed)
                std::reverse(pads.begin(), pads.end());
            for (const auto& pad : pads)
                registry.observe(pad);
        };

        PhysicalControllerRegistry forward;
        PhysicalControllerRegistry backward;
        populate(forward, false);
        populate(backward, true);

        ProviderObservation probe{ControllerProvider::XInput, QStringLiteral("x-1")};
        probe.containerId = QStringLiteral("container-3");
        const QString forwardId = forward.observe(probe);
        const QString backwardId = backward.observe(probe);
        QCOMPARE(forwardId, backwardId);
        QCOMPARE(forward.matchEvidence(ControllerProvider::XInput, QStringLiteral("x-1")),
                 MatchEvidence::ContainerId);
        QCOMPARE(backward.matchEvidence(ControllerProvider::XInput, QStringLiteral("x-1")),
                 MatchEvidence::ContainerId);
    }

    void reconnectRestoresTheSameLogicalIdentity()
    {
        PhysicalControllerRegistry registry;
        ProviderObservation pad{ControllerProvider::GameInput, QStringLiteral("gi-1")};
        pad.appLocalDeviceId = QStringLiteral("app-1");
        pad.capabilities = ControllerCapability::StandardControls;
        const QString id = registry.observe(pad);

        QVERIFY(registry.removeProvider(ControllerProvider::GameInput,
                                        QStringLiteral("gi-1")));
        QVERIFY(!registry.controller(id)->connected());

        QCOMPARE(registry.observe(pad), id);
        QVERIFY(registry.controller(id)->connected());
        QCOMPARE(registry.controllers().size(), 1);
        QCOMPARE(registry.matchEvidence(ControllerProvider::GameInput,
                                        QStringLiteral("gi-1")),
                 MatchEvidence::AppLocalDeviceId);
    }

    void firstObservationRecordsNewIdentityProvenance()
    {
        PhysicalControllerRegistry registry;
        ProviderObservation pad{ControllerProvider::XInput, QStringLiteral("x-1")};
        pad.topologyRoot = QStringLiteral("root-r");
        registry.observe(pad);
        QCOMPARE(registry.matchEvidence(ControllerProvider::XInput, QStringLiteral("x-1")),
                 MatchEvidence::NewIdentity);

        ProviderObservation paired{ControllerProvider::GameInput, QStringLiteral("gi-1")};
        paired.topologyRoot = pad.topologyRoot;
        registry.observe(paired);
        QCOMPARE(registry.matchEvidence(ControllerProvider::GameInput, QStringLiteral("gi-1")),
                 MatchEvidence::TopologyRoot);
    }

    void reObservationRefreshesAttachmentCapabilities()
    {
        PhysicalControllerRegistry registry;
        ProviderObservation gameInput{ControllerProvider::GameInput, QStringLiteral("gi-1")};
        gameInput.appLocalDeviceId = QStringLiteral("app-1");
        gameInput.capabilities = ControllerCapability::StandardControls;
        const QString id = registry.observe(gameInput);
        QVERIFY(!registry.controller(id)->capabilities()
                     .testFlag(ControllerCapability::SystemShare));

        // CapabilityChanged re-observes the same attachment with new flags.
        gameInput.capabilities = ControllerCapability::StandardControls
            | ControllerCapability::SystemShare;
        QCOMPARE(registry.observe(gameInput), id);
        QCOMPARE(registry.controller(id)->providers.size(), 1);
        QVERIFY(registry.controller(id)->capabilities()
                    .testFlag(ControllerCapability::SystemShare));
    }

    void providerRemovalKeepsRemainingLogicalController()
    {
        PhysicalControllerRegistry registry;
        ProviderObservation one{ControllerProvider::XInput, QStringLiteral("x")};
        one.containerId = QStringLiteral("same");
        ProviderObservation two{ControllerProvider::GameInput, QStringLiteral("g")};
        two.containerId = one.containerId;
        const QString id = registry.observe(one);
        registry.observe(two);

        QVERIFY(registry.removeProvider(ControllerProvider::XInput, QStringLiteral("x")));
        QVERIFY(registry.controller(id));
        QCOMPARE(registry.controller(id)->providers.size(), 1);
    }
};

QTEST_MAIN(PhysicalControllerRegistryTest)
#include "tst_physicalcontrollerregistry.moc"
