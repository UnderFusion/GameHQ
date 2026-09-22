#include "gameinput/FakeGameInputApi.h"
#include "gameinput/GameInputFocusController.h"
#include "gameinput/GameInputLabelMap.h"
#include "gameinput/GameInputRouter.h"
#include "input/ControlId.h"
#include "input/BindingPattern.h"
#include "input/ExtraButtonCatalog.h"
#include "input/ProviderIntegration.h"
#include "storage/CaptureDatabase.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "GameInput.h"

#include <algorithm>
using namespace ModernInput;

namespace GI = GameInput::v3;

namespace {
GameInputEvent makeEvent(GameInputEventKind kind, const QString& control = {})
{
    GameInputEvent result;
    result.kind = kind;
    result.deviceId = QStringLiteral("device-a");
    result.controlId = control;
    result.device.deviceId = result.deviceId;
    result.device.rootId = QStringLiteral("pnp.root.device-a");
    result.device.containerId = QStringLiteral("container-a");
    result.device.displayName = QStringLiteral("Modern test pad");
    result.device.vendorId = 0x1234;
    result.device.productId = 0x5678;
    result.device.supportedSystemButtons = 3;
    return result;
}
}

class GameInputRouterTest : public QObject
{
    Q_OBJECT

private slots:
    void degradedSystemButtonsPreserveFallbackAndDeviceIdentity()
    {
        ProviderIntegration integration;
        const auto caps = ControllerCapability::StandardControls
            | ControllerCapability::SystemShare | ControllerCapability::Guide;
        const QString legacyId = integration.observeLegacy(ControllerProvider::SonyRaw,
            QStringLiteral("physical"), {}, QStringLiteral("DualSense"), caps,
            nullptr, {}, {}, QStringLiteral("pnp.root.device-a"));
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        raw->setRegistrationFailure(FakeGameInputApi::Kind::System);
        GameInputFocusController focus;
        GameInputRouter router(std::move(api));
        router.setProviderIntegration(&integration);
        router.setFocusController(&focus);
        QSignalSpy connected(&router, &GameInputRouter::deviceConnected);
        QSignalSpy disconnected(&router, &GameInputRouter::deviceDisconnected);
        QSignalSpy fallback(&router, &GameInputRouter::sessionFallback);
        QVERIFY(router.start());
        auto physical = makeEvent(GameInputEventKind::DeviceAdded);
        physical.device.containerId.clear(); // correlate by root, upgrade to app-local identity
        raw->emitDevice(physical);
        QTRY_COMPARE(connected.size(), 1);
        const QString rootId = router.registry().logicalIdFor(ControllerProvider::GameInput, physical.deviceId);
        QVERIFY(!rootId.isEmpty());
        QVERIFY(rootId != legacyId);
        physical.kind = GameInputEventKind::CapabilityChanged;
        physical.device.containerId = QStringLiteral("container-a");
        raw->emitDevice(physical);
        QTRY_COMPARE(router.registry().controller(router.registry().logicalIdFor(
            ControllerProvider::GameInput, physical.deviceId))->containerId, QStringLiteral("container-a"));
        const QString physicalId = router.registry().logicalIdFor(ControllerProvider::GameInput, physical.deviceId);
        QCOMPARE(physicalId, rootId); // metadata enrichment must not create another controller
        QCOMPARE(router.registry().logicalIdFor(ControllerProvider::SonyRaw, QStringLiteral("physical")), physicalId);
        QVERIFY(focus.requestExclusiveForeground(QStringLiteral("test-interactive")));

        for (int cycle = 0; cycle < 2; ++cycle) {
            auto virtualPad = makeEvent(GameInputEventKind::DeviceAdded);
            virtualPad.deviceId = QStringLiteral("virtual-%1").arg(cycle);
            virtualPad.device.deviceId = virtualPad.deviceId;
            virtualPad.device.containerId = QStringLiteral("virtual-container");
            virtualPad.device.rootId = QStringLiteral("virtual-root");
            raw->emitDevice(virtualPad);
            QTRY_COMPARE(connected.size(), cycle + 2);
            const QString virtualId = router.registry().logicalIdFor(ControllerProvider::GameInput, virtualPad.deviceId);
            QVERIFY(virtualId != physicalId);
            virtualPad.kind = GameInputEventKind::DeviceRemoved;
            raw->emitDevice(virtualPad);
            QTRY_COMPARE(disconnected.size(), cycle + 1);
            QVERIFY(!router.registry().controller(virtualId)->connected());
            QCOMPARE(router.registry().logicalIdFor(ControllerProvider::GameInput, physical.deviceId), physicalId);
            QVERIFY(focus.exclusiveForegroundActive());
            QVERIFY(router.active());

            physical.kind = GameInputEventKind::Reading;
            physical.standardButtons = cycle + 1;
            raw->emitReading(physical);
            QTRY_COMPARE(router.shadowReadingCount(), cycle + 1);
            for (const QString& control : {ControlId::Capture, ControlId::Guide}) {
                QVERIFY(integration.routeLegacySystemEdge(ControllerProvider::SonyRaw,
                    QStringLiteral("physical"), control, true, 1000 + cycle * 100).accepted);
                QVERIFY(integration.routeLegacySystemEdge(ControllerProvider::SonyRaw,
                    QStringLiteral("physical"), control, false, 1050 + cycle * 100).accepted);
            }
        }
        QCOMPARE(fallback.size(), 0);
        QVERIFY(!raw->callLog().contains(QStringLiteral("unload")));
        router.shutdown();
        QVERIFY(!focus.policyAttached());
        QVERIFY(!focus.exclusiveForegroundActive());
    }

    void offNeverLoadsTheRuntime()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api), GameInputRouter::SupportMode::Off);
        QVERIFY(router.start());
        QCOMPARE(router.runtimeStatus(), QStringLiteral("Off"));
        QVERIFY(raw->callLog().isEmpty());
    }

    // cpo-o06c: a session with GameInput switched off has no focus policy to
    // change — the legacy providers (Sony Raw, XInput, WinMM) are not touched by
    // this leaf at all, and an installed owner must not invent a policy request
    // for a runtime that was never loaded.
    void offModeNeverTouchesTheFocusPolicy()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        auto controller = std::make_unique<GameInputFocusController>();
        GameInputFocusController* focus = controller.get();
        GameInputRouter router(std::move(api), GameInputRouter::SupportMode::Off);
        router.setFocusController(focus);

        QVERIFY(router.start());
        QVERIFY(raw->callLog().isEmpty());
        QVERIFY(!focus->policyAttached());

        // A request in this state is refused and recorded, never queued.
        QVERIFY(!focus->requestExclusiveForeground(QStringLiteral("overlay-interactive")));
        QVERIFY(focus->focusMode() == GameInputFocusMode::Background);
        QVERIFY(raw->callLog().isEmpty());

        router.shutdown();
        QVERIFY(raw->callLog().isEmpty());
    }

    // cpo-o06c: switching GameInput off while the overlay holds the exclusive
    // policy must return the runtime to the background policy on the way out —
    // a provider/runtime disconnect can never leave the process exclusive.
    void switchingTheRuntimeOffReleasesTheExclusivePolicy()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        auto controller = std::make_unique<GameInputFocusController>();
        GameInputFocusController* focus = controller.get();
        GameInputRouter router(std::move(api));
        router.setFocusController(focus);

        QVERIFY(router.start());
        QVERIFY(focus->policyAttached());
        QVERIFY(focus->requestExclusiveForeground(QStringLiteral("overlay-interactive")));
        QVERIFY(focus->exclusiveForegroundActive());

        router.setMode(GameInputRouter::SupportMode::Off);

        QVERIFY(focus->focusMode() == GameInputFocusMode::Background);
        QVERIFY(!focus->policyAttached());
        const QStringList log = raw->callLog();
        QVERIFY(log.lastIndexOf(QStringLiteral("focus-policy:background"))
                > log.lastIndexOf(QStringLiteral("focus-policy:exclusive-foreground")));
    }

    void readingsStayShadowWhileSystemButtonsRoute()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        QSignalSpy pressed(&router, &GameInputRouter::systemControlPressed);
        QSignalSpy released(&router, &GameInputRouter::systemControlReleased);
        QVERIFY(router.start());

        raw->emitDevice(makeEvent(GameInputEventKind::DeviceAdded));
        raw->emitReading(makeEvent(GameInputEventKind::Reading));
        QTRY_COMPARE(router.shadowReadingCount(), 1);
        QCOMPARE(pressed.size(), 0);

        raw->emitSystem(makeEvent(GameInputEventKind::SystemButtonPressed, ControlId::Capture));
        raw->emitSystem(makeEvent(GameInputEventKind::SystemButtonReleased, ControlId::Capture));
        QTRY_COMPARE(pressed.size(), 1);
        QTRY_COMPARE(released.size(), 1);
        QCOMPARE(pressed.at(0).at(0).toString(), ControlId::Capture);
        QVERIFY(!pressed.at(0).at(1).toString().isEmpty());
    }

    void uncertainStateReleasesHeldControlsAndFallsBack()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        QSignalSpy released(&router, &GameInputRouter::systemControlReleased);
        QSignalSpy fallback(&router, &GameInputRouter::sessionFallback);
        QVERIFY(router.start());
        raw->emitSystem(makeEvent(GameInputEventKind::SystemButtonPressed, ControlId::Guide));
        QTRY_VERIFY(router.active());
        raw->emitDevice(makeEvent(GameInputEventKind::RecoveryRequired));

        QTRY_COMPARE(fallback.size(), 1);
        QCOMPARE(released.size(), 1);
        QVERIFY(router.failedForSession());
        QVERIFY(!router.active());
        QVERIFY(raw->callLog().contains(QStringLiteral("unload")));
    }

    void extraButtonAbove32RoutesWithAStableDeviceLocalId()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        QSignalSpy pressed(&router, &GameInputRouter::systemControlPressed);
        QSignalSpy released(&router, &GameInputRouter::systemControlReleased);
        QVERIFY(router.start());

        auto reading = makeEvent(GameInputEventKind::Reading);
        reading.buttonStates.fill(0, 40);
        reading.device.extraButtonCount = 40;
        raw->emitReading(reading);
        QTRY_COMPARE(router.shadowReadingCount(), 1);
        reading.buttonStates[39] = 1;
        raw->emitReading(reading);
        QTRY_COMPARE(pressed.size(), 1);
        const QString control = pressed.at(0).at(0).toString();
        QVERIFY(ControlId::isDeviceButton(control));
        QCOMPARE(ControlId::label(control, ControlId::ControllerFamily::Generic),
                 QStringLiteral("Extra Button 40"));

        reading.buttonStates[39] = 0;
        raw->emitReading(reading);
        QTRY_COMPARE(released.size(), 1);
        QCOMPARE(released.at(0).at(0).toString(), control);
    }

    void standardReadingsStayShadowUntilCrossProviderDedup()
    {
        // Sony/XInput/WinMM still deliver standard controls through the
        // legacy arbitration and do not feed PhysicalControllerRegistry:
        // routing GameInput standard readings too would double-fire one
        // physical press. They must stay shadow-only until t25's real
        // cross-provider integration lands.
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        QSignalSpy pressed(&router, &GameInputRouter::systemControlPressed);
        QSignalSpy released(&router, &GameInputRouter::systemControlReleased);
        QVERIFY(router.start());
        auto reading = makeEvent(GameInputEventKind::Reading);
        reading.standardButtons = 0x00000004u; // GameInputGamepadA
        raw->emitReading(reading);
        QTRY_COMPARE(router.shadowReadingCount(), 1);
        reading.standardButtons = 0;
        raw->emitReading(reading);
        QTRY_COMPARE(router.shadowReadingCount(), 2);
        QCOMPARE(pressed.size(), 0);
        QCOMPARE(released.size(), 0);
    }

    void guideOnlyPadDoesNotReportShare()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        QVERIFY(router.start());
        auto added = makeEvent(GameInputEventKind::DeviceAdded);
        added.device.supportedSystemButtons = 0x1; // GameInputSystemButtonGuide only
        raw->emitDevice(added);
        QTRY_VERIFY(router.compatibilityReport()
                        .contains(QStringLiteral("Guide: Available")));
        QVERIFY(router.compatibilityReport()
                    .contains(QStringLiteral("Share: Not reported")));
    }

    void staleReadingAfterRemovalDoesNotResurrectDevice()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        QSignalSpy connected(&router, &GameInputRouter::deviceConnected);
        QSignalSpy disconnected(&router, &GameInputRouter::deviceDisconnected);
        QVERIFY(router.start());

        raw->emitDevice(makeEvent(GameInputEventKind::DeviceAdded));
        QTRY_COMPARE(connected.size(), 1);
        raw->emitDevice(makeEvent(GameInputEventKind::DeviceRemoved));
        QTRY_COMPARE(disconnected.size(), 1);

        // A reading that trails the removal (late callback) must not
        // re-register the device. The second pad is only a sentinel proving
        // the batch containing the stale reading was fully processed.
        raw->emitReading(makeEvent(GameInputEventKind::Reading));
        auto other = makeEvent(GameInputEventKind::DeviceAdded);
        other.deviceId = QStringLiteral("device-b");
        other.device.deviceId = other.deviceId;
        other.device.containerId = QStringLiteral("container-b");
        raw->emitDevice(other);
        QTRY_COMPARE(connected.size(), 2);
        QCOMPARE(router.shadowReadingCount(), 0);
        QVERIFY(router.registry()
                    .logicalIdFor(ControllerProvider::GameInput,
                                  QStringLiteral("device-a")).isEmpty());

        // A genuine re-add clears the tombstone and readings flow again.
        raw->emitDevice(makeEvent(GameInputEventKind::DeviceAdded));
        QTRY_COMPARE(connected.size(), 3);
        raw->emitReading(makeEvent(GameInputEventKind::Reading));
        QTRY_COMPARE(router.shadowReadingCount(), 1);
    }

    void changedLayoutReleasesHeldAndBlocksUntilConfirmed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CaptureDatabase db(dir.filePath(QStringLiteral("layout.db")));
        QVERIFY(db.open());
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api), GameInputRouter::SupportMode::Auto, &db);
        QSignalSpy pressed(&router, &GameInputRouter::systemControlPressed);
        QSignalSpy released(&router, &GameInputRouter::systemControlReleased);
        QVERIFY(router.start());

        auto reading = makeEvent(GameInputEventKind::Reading);
        reading.device.extraButtonCount = 4;
        reading.buttonStates = {0, 0, 0, 0};
        raw->emitReading(reading);
        QTRY_COMPARE(router.shadowReadingCount(), 1);
        reading.buttonStates = {1, 0, 0, 0};
        raw->emitReading(reading);
        QTRY_COMPARE(pressed.size(), 1);
        const QString oldControl = pressed.at(0).at(0).toString();

        // Firmware/mode switch while the old control is held: the old layout
        // must be released and the new one must stay silent until the user
        // reconfirms it — an old index must never fire with a new meaning.
        reading.device.extraButtonCount = 3;
        reading.buttonStates = {1, 0, 0};
        raw->emitReading(reading);
        QTRY_COMPARE(released.size(), 1);
        QCOMPARE(released.at(0).at(0).toString(), oldControl);
        QVERIFY(router.layoutWarning());

        reading.buttonStates = {0, 1, 0};
        raw->emitReading(reading);
        QTRY_COMPARE(router.shadowReadingCount(), 4);
        QCOMPARE(pressed.size(), 1);
    }

    void layoutConfirmationIsPerControllerAndListsStaleAssignments()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CaptureDatabase db(dir.filePath(QStringLiteral("layout-multi.db")));
        QVERIFY(db.open());
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api), GameInputRouter::SupportMode::Auto, &db);
        QVERIFY(router.start());

        auto padA = makeEvent(GameInputEventKind::Reading);
        padA.device.extraButtonCount = 2;
        padA.buttonStates = {0, 0};
        raw->emitReading(padA);
        auto padB = padA;
        padB.deviceId = QStringLiteral("device-b");
        padB.device.deviceId = padB.deviceId;
        padB.device.containerId = QStringLiteral("container-b");
        padB.device.displayName = QStringLiteral("Second test pad");
        raw->emitReading(padB);
        QTRY_COMPARE(router.shadowReadingCount(), 2);

        const QString logicalA = router.registry().logicalIdFor(
            ControllerProvider::GameInput, padA.deviceId);
        const QString logicalB = router.registry().logicalIdFor(
            ControllerProvider::GameInput, padB.deviceId);
        QVERIFY(!logicalA.isEmpty());
        QVERIFY(!logicalB.isEmpty());
        const QString oldA = ControlId::deviceButton(
            logicalA, ExtraButtonCatalog::signatureFor(2, {}), 0);
        const QString oldB = ControlId::deviceButton(
            logicalB, ExtraButtonCatalog::signatureFor(2, {}), 1);
        QVERIFY(db.upsertBindingOverride(
            {QStringLiteral("controller"), logicalA,
             QStringLiteral("desktop.favorite"), 2,
             TriggerSpec::orderedChord(oldA, ControlId::Guide).serialize(),
             QStringLiteral("press"), 0, false}));
        QVERIFY(db.upsertBindingOverride(
            {QStringLiteral("controller"), logicalB,
             QStringLiteral("desktop.menu"), 2, oldB,
             QStringLiteral("press"), 0, false}));

        padA.device.extraButtonCount = 3;
        padA.buttonStates = {0, 0, 0};
        raw->emitReading(padA);
        padB.device.extraButtonCount = 4;
        padB.buttonStates = {0, 0, 0, 0};
        raw->emitReading(padB);
        QTRY_COMPARE(router.layoutWarnings().size(), 2);

        const auto warnings = router.layoutWarnings();
        const auto warningA = std::find_if(warnings.cbegin(), warnings.cend(),
            [&](const auto& warning) { return warning.logicalId == logicalA; });
        const auto warningB = std::find_if(warnings.cbegin(), warnings.cend(),
            [&](const auto& warning) { return warning.logicalId == logicalB; });
        QVERIFY(warningA != warnings.cend());
        QVERIFY(warningB != warnings.cend());
        QVERIFY(warningA->staleAssignments.join(QLatin1Char(' '))
                    .contains(QStringLiteral("Favorite")));
        QVERIFY(warningB->staleAssignments.join(QLatin1Char(' '))
                    .contains(QStringLiteral("Menu")));

        QVERIFY(router.confirmLayout(logicalA));
        QCOMPARE(router.layoutWarnings().size(), 1);
        QCOMPARE(router.layoutWarnings().front().logicalId, logicalB);
        QVERIFY(router.layoutWarning());
        QVERIFY(!router.confirmLayout(logicalA));

        // Confirmation enables the new namespace; it never rewrites the old
        // binding row, which therefore still requires explicit reassignment.
        const auto rows = db.listBindingOverrides();
        QVERIFY(std::any_of(rows.cbegin(), rows.cend(),
            [&](const auto& row) { return row.triggerCode.contains(oldA); }));
        QVERIFY(router.confirmLayout(logicalB));
        QVERIFY(!router.layoutWarning());
    }

    void disconnectReleasesHeldStateAndStrongReconnectRestoresIdentity()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        QSignalSpy connected(&router, &GameInputRouter::deviceConnected);
        QSignalSpy disconnected(&router, &GameInputRouter::deviceDisconnected);
        QSignalSpy reset(&router, &GameInputRouter::lifecycleReset);
        QSignalSpy pressed(&router, &GameInputRouter::systemControlPressed);
        QSignalSpy released(&router, &GameInputRouter::systemControlReleased);
        QVERIFY(router.start());

        raw->emitDevice(makeEvent(GameInputEventKind::DeviceAdded));
        QTRY_COMPARE(connected.size(), 1);
        const QString logicalId = connected.at(0).at(0).toString();
        QVERIFY(!connected.at(0).at(1).toBool());
        raw->emitSystem(makeEvent(GameInputEventKind::SystemButtonPressed, ControlId::Guide));
        QTRY_COMPARE(pressed.size(), 1);
        raw->emitDevice(makeEvent(GameInputEventKind::DeviceRemoved));
        QTRY_COMPARE(disconnected.size(), 1);
        QCOMPARE(reset.size(), 1);
        QCOMPARE(released.size(), 1);
        QVERIFY(!router.registry().controller(logicalId)->connected());

        raw->emitDevice(makeEvent(GameInputEventKind::DeviceAdded));
        QTRY_COMPARE(connected.size(), 2);
        QCOMPARE(connected.at(1).at(0).toString(), logicalId);
        QVERIFY(connected.at(1).at(1).toBool());

        // There was no physical release before removal. Router reset must
        // forget the stale pressed edge so the first reconnect press routes.
        raw->emitSystem(makeEvent(GameInputEventKind::SystemButtonPressed,
                                  ControlId::Guide));
        QTRY_COMPARE(pressed.size(), 2);
    }

    void sleepingOneOfTwoDevicesReleasesOnlyThatDevice()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        QSignalSpy pressed(&router, &GameInputRouter::systemControlPressed);
        QSignalSpy released(&router, &GameInputRouter::systemControlReleased);
        QVERIFY(router.start());
        auto first = makeEvent(GameInputEventKind::SystemButtonPressed, ControlId::Guide);
        auto second = first;
        second.deviceId = QStringLiteral("device-b");
        second.device.deviceId = second.deviceId;
        second.device.containerId = QStringLiteral("container-b");
        raw->emitSystem(first);
        raw->emitSystem(second);
        QTRY_COMPARE(pressed.size(), 2);

        first.kind = GameInputEventKind::Sleep;
        raw->emitDevice(first);
        QTRY_COMPARE(released.size(), 1);
        QCOMPARE(released.at(0).at(1).toString(),
                 router.registry().logicalIdFor(ControllerProvider::GameInput,
                                                QStringLiteral("device-a")));
    }

    void sharedIntegrationDedupsAgainstCorrelatedLegacyProvider()
    {
        ProviderIntegration integration;
        integration.observeLegacy(ControllerProvider::SonyRaw,
                                  QStringLiteral("1234:5678"), QStringLiteral("1234:5678"),
                                  QStringLiteral("DualSense"),
                                  ControllerCapability::StandardControls
                                      | ControllerCapability::SystemShare
                                      | ControllerCapability::Guide, nullptr,
                                  {}, {}, QStringLiteral("pnp.root.device-a"));
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        router.setProviderIntegration(&integration);
        QSignalSpy pressed(&router, &GameInputRouter::systemControlPressed);
        QVERIFY(router.start());

        // makeEvent's pad is VID 1234 / PID 5678 — same topology root as the
        // legacy attachment, so the two providers merge onto one controller.
        raw->emitDevice(makeEvent(GameInputEventKind::DeviceAdded));
        raw->emitSystem(makeEvent(GameInputEventKind::SystemButtonPressed, ControlId::Capture));
        QTRY_COMPARE(pressed.size(), 1);
        const QString logicalId = pressed.at(0).at(1).toString();
        QVERIFY(integration.hasLegacyAttachment(logicalId));

        // The mirrored legacy edge for the same physical press is refused —
        // exactly one Capture edge for one physical Share press.
        const auto legacy = integration.routeLegacySystemEdge(
            ControllerProvider::SonyRaw, QStringLiteral("1234:5678"),
            ControlId::Capture, true, 5);
        QVERIFY(!legacy.accepted);

        raw->emitSystem(makeEvent(GameInputEventKind::SystemButtonReleased, ControlId::Capture));
        QTRY_COMPARE(router.shadowedSystemEdgeCount(), 0);

        // Off detaches GameInput from the shared registry: the legacy path
        // must own Share again immediately.
        router.setMode(GameInputRouter::SupportMode::Off);
        const auto legacyAfterOff = integration.routeLegacySystemEdge(
            ControllerProvider::SonyRaw, QStringLiteral("1234:5678"),
            ControlId::Capture, true, 500);
        QVERIFY(legacyAfterOff.accepted);
    }

    void uncorrelatedLegacyProviderShadowsSystemButtons()
    {
        ProviderIntegration integration;
        // A legacy pad with a DIFFERENT identity than the GameInput device:
        // it could be the same physical controller behind a remapper, so
        // dedup cannot be proven and GameInput Share/Guide must stay shadow.
        integration.observeLegacy(ControllerProvider::SonyRaw,
                                  QStringLiteral("054c:0ce6"), QStringLiteral("054c:0ce6"),
                                  QStringLiteral("DualSense"),
                                  ControllerCapability::StandardControls
                                      | ControllerCapability::SystemShare
                                      | ControllerCapability::Guide);
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        router.setProviderIntegration(&integration);
        QSignalSpy pressed(&router, &GameInputRouter::systemControlPressed);
        QVERIFY(router.start());

        raw->emitDevice(makeEvent(GameInputEventKind::DeviceAdded));
        raw->emitSystem(makeEvent(GameInputEventKind::SystemButtonPressed, ControlId::Capture));
        raw->emitSystem(makeEvent(GameInputEventKind::SystemButtonReleased, ControlId::Capture));
        QTRY_VERIFY(router.shadowedSystemEdgeCount() >= 1);
        QCOMPARE(pressed.size(), 0);

        // Legacy Capture keeps working through its own dedup route.
        const auto legacy = integration.routeLegacySystemEdge(
            ControllerProvider::SonyRaw, QStringLiteral("054c:0ce6"),
            ControlId::Capture, true, 10);
        QVERIFY(legacy.accepted);
    }

    void autoOffAutoRestartStressTwentyCycles()
    {
        ProviderIntegration integration;
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        router.setProviderIntegration(&integration);
        QSignalSpy pressed(&router, &GameInputRouter::systemControlPressed);
        QSignalSpy released(&router, &GameInputRouter::systemControlReleased);
        QVERIFY(router.start());

        for (int cycle = 0; cycle < 20; ++cycle) {
            raw->emitDevice(makeEvent(GameInputEventKind::DeviceAdded));
            raw->emitSystem(makeEvent(GameInputEventKind::SystemButtonPressed,
                                      ControlId::Capture));
            // Deliberately do NOT send the physical release. Off must end the
            // held generation and synthesize it before Auto starts again.
            QTRY_COMPARE(pressed.size(), cycle + 1);

            router.setMode(GameInputRouter::SupportMode::Off);
            QTRY_COMPARE(released.size(), cycle + 1);
            // A late callback after Off must be swallowed, not queued for the
            // next session and not delivered as a duplicate action.
            raw->emitRetired(makeEvent(GameInputEventKind::SystemButtonPressed,
                                       ControlId::Capture));
            QCOMPARE(pressed.size(), cycle + 1);
            // Off detaches the shared-registry attachment each cycle.
            QVERIFY(integration.registry()
                        .logicalIdFor(ControllerProvider::GameInput,
                                      QStringLiteral("device-a")).isEmpty());
            router.setMode(GameInputRouter::SupportMode::Auto);
            QVERIFY(router.active());
        }
        // One live callback registration set: every register call from the 20
        // restarts has a matching stop/unregister except the final session's.
        const QStringList log = raw->callLog();
        int registers = 0;
        int unregisters = 0;
        for (const QString& entry : log) {
            if (entry.startsWith(QLatin1String("register:")))
                ++registers;
            else if (entry.startsWith(QLatin1String("unregister:")))
                ++unregisters;
        }
        QCOMPARE(registers - unregisters, 3);   // device + reading + system
    }

    void extraCatalogBlanksStandardAndSystemEnums()
    {
        ExtraButtonCatalog catalog;   // no database: pure layout logic
        const auto layout = catalog.observe(
            QStringLiteral("controller-test"), 4,
            {GameInputLabelMap::describe(GI::GameInputLabelXboxA),
             GameInputLabelMap::describe(GI::GameInputLabelLetterM),
             GameInputLabelMap::describe(GI::GameInputLabelL3),
             GameInputLabelMap::describe(GI::GameInputLabelShare)});
        QCOMPARE(layout.controlIds.size(), 4);
        QVERIFY(layout.controlIds.at(0).isEmpty());   // A = standard face button
        QVERIFY(!layout.controlIds.at(1).isEmpty());  // M1 = genuine extra
        QVERIFY(layout.controlIds.at(2).isEmpty());   // L3 = standard
        QVERIFY(layout.controlIds.at(3).isEmpty());   // Share = system button
        // Index alignment with the reading's state array is preserved.
        QCOMPARE(layout.labels.size(), 4);
    }

    void standardLabeledExtraButtonNeverRoutes()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        QSignalSpy pressed(&router, &GameInputRouter::systemControlPressed);
        QVERIFY(router.start());

        auto reading = makeEvent(GameInputEventKind::Reading);
        reading.buttonStates.fill(0, 2);
        reading.device.extraButtonCount = 1;
        reading.device.buttons = {
            GameInputLabelMap::describe(GI::GameInputLabelXboxA),
            GameInputLabelMap::describe(GI::GameInputLabelLetterM)};
        raw->emitReading(reading);
        QTRY_COMPARE(router.shadowReadingCount(), 1);

        // The standard-labeled index toggling must not produce an extra edge;
        // the genuine extra still routes.
        reading.buttonStates[0] = 1;
        reading.buttonStates[1] = 1;
        raw->emitReading(reading);
        QTRY_COMPARE(pressed.size(), 1);
        QCOMPARE(ControlId::label(pressed.at(0).at(0).toString(),
                                  ControlId::ControllerFamily::Generic),
                 QStringLiteral("Extra Button 2"));
    }

    void compatibilityReportIsUsefulAndRedacted()
    {
        auto api = std::make_unique<FakeGameInputApi>();
        auto* raw = api.get();
        GameInputRouter router(std::move(api));
        QVERIFY(router.start());
        raw->emitDevice(makeEvent(GameInputEventKind::DeviceAdded));
        QTRY_VERIFY(router.controllerSummary().contains(QStringLiteral("Modern test pad")));
        const QString report = router.compatibilityReport();
        QVERIFY(report.contains(QStringLiteral("Anonymous ID:")));
        QVERIFY(report.contains(QStringLiteral("Providers: GameInput")));
        QVERIFY(report.contains(QStringLiteral("Share: Available")));
        QVERIFY(!report.contains(QStringLiteral("container-a")));
        QVERIFY(!report.contains(QStringLiteral("device-a")));
    }
};

QTEST_MAIN(GameInputRouterTest)
#include "tst_gameinputrouter.moc"
