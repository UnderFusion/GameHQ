#pragma once
#include "input/Gamepad.h"
#include "input/InputDiagnostics.h"
#include "input/InputRateMonitor.h"
#include "input/RawInputApi.h"

#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <memory>

class QTimer;

// Sony pad reader over Win32 Raw Input. Handles DualSense, DualSense Edge,
// DS4, and DSX/ViGEm virtual DS4 reports.
//
// A message-only window receives WM_INPUT with RIDEV_INPUTSINK so pad input
// arrives even while a game has focus, and WM_INPUT_DEVICE_CHANGE via
// RIDEV_DEVNOTIFY for hot-plug. Every supported device is tracked with its
// OWN state keyed by Raw Input handle (handles change on every reconnect),
// so two pads reporting at once — e.g. a real DualSense next to DSX's
// virtual DS4 — can't corrupt each other's edge detection. One device is
// "active" at a time; if it is removed or goes silent while another tracked
// pad shows real input, the active role fails over immediately instead of
// waiting out the disconnect debounce.
//
// USB (report 0x01) and Bluetooth (simple 0x01, full 0x31) layouts are all
// parsed; offsets live in SonyReportLayout.h. HID collections whose path
// contains "IG_" are XInput devices and are left to the XInput backend.
// See docs/controller-input.md.
// Win32 lives in the .cpp only.
//
// Every WM_INPUT is read header-first and classified once per handle: a device
// this backend does not drive — an 8000 Hz gamepad in XInput mode, say — costs
// one header read and one hash lookup for the rest of its life, never a
// payload fetch on the GUI thread. All OS queries go through the injectable
// RawInputApi seam so that guarantee is testable.
class DualSenseDevice : public Gamepad
{
    Q_OBJECT
public:
    explicit DualSenseDevice(QObject* parent = nullptr);
    // Test seam: takes ownership of `api`. Production uses the constructor
    // above, which installs RawInputApi::createSystem().
    explicit DualSenseDevice(RawInputApi* api, QObject* parent = nullptr);
    ~DualSenseDevice() override;

    bool start() override;

    // Pure Sony HID button-block decode, exposed so the canonical control
    // each physical button produces can be compared against the WinMM and
    // XInput views of the same pad without a real device.
    static quint32 decodeButtons(const unsigned char* d, int base);
    ControlId::DeviceProfile profile() const override;

    // Re-run the debounced device reconciliation (used after the HidHide
    // whitelist fix so a newly-visible pad is picked up promptly).
    void rescan();

    // "Press your screenshot button now" diagnostics probe: while the
    // InputDiagnostics window is open, ignored gamepad-usage handles get their
    // payloads read and diffed too, so a button GameHQ never delivers still
    // shows up as changed report bytes. Strictly bounded (payload-read cap,
    // one eligibility query per handle per probe) and self-cleaning — the
    // per-event fast path returns to one hash lookup when the window closes.
    void beginButtonProbe();

    // Distinct "vvvv:pppp" identities of the XInput-class (IG_) HID
    // collections Raw Input currently sees. This backend never drives them,
    // but it is the only one that knows their real hardware identity — the
    // XInput API itself only exposes slot numbers. Feeds the stable-identity
    // correlation in InputEngine (ControllerIdentity).
    QStringList xinputClassIdentities() const;
    QStringList xinputClassEndpoints() const;

    // Called from the window procedure — not for general use.
    void noteWmInput(bool sink) { ++(sink ? m_wmInputSink : m_wmInputForeground); }
    void onRawInput(void* hRawInput);
    void onDeviceChange(bool arrived, void* deviceHandle);

signals:
    // Debounced hint that the HID device topology changed (any arrival or
    // removal, including XInput/unsupported devices — Windows re-enumerates
    // the whole tree in bursts). InputEngine forwards it to the XInput and
    // WinMM backends so they can rescan on events instead of continuously
    // polling empty slots (XInputGetState on an empty slot can stall for
    // milliseconds — see docs/controller-input.md).
    void deviceTopologyChanged();

    // Fired (on change only) when supported pads exist in Windows PnP but are
    // invisible to Raw Input — i.e. cloaked by a HID filter driver such as
    // HidHide. Empty list = previously hidden pads are visible again.
    void hiddenPadsChanged(const QStringList& padNames, bool hidHidePresent);

    // Production selective-Raw-HID path (t26): a bound (or probe-observed)
    // HID button usage on a gamepad-class device this backend does NOT drive
    // changed state. `deviceIdentity` is a stable anonymized endpoint identity;
    // `controlId` is the canonical ControlId::rawHidUsage code bindings
    // persist. InputEngine routes these through ProviderIntegration into the
    // binding runtime.
    void rawHidControl(const QString& deviceIdentity, const QString& controlId,
                       bool pressed);
    // The final live Raw Input handle for this fallback identity disappeared;
    // ProviderIntegration must evict the attachment after held releases have
    // been synthesized.
    void rawHidDeviceRemoved(const QString& deviceIdentity);

private:
    struct DeviceState {
        int layout = 0;             // ReportLayout (DualSense / DS4)
        quint32 vendorId = 0;
        quint32 productId = 0;
        QString path;               // RIDI_DEVICENAME — stable device identity
        quint32 buttons = 0;        // last parsed button+stick bitmask
        quint32 stick = 0;          // stick-derived direction bits (hysteresis)
        qint64 lastReportMs = 0;    // m_clock timestamp of the last report
        qint64 lastChangeMs = 0;    // last real button/stick edge, not idle traffic
        bool reported = false;      // produced at least one valid report
        int reportShape = -1;       // (report id << 16) | length last logged
    };

    bool registerRawInput(bool remove = false);
    DeviceState* probeDevice(void* handle);   // query + track a handle; null if unsupported
    DeviceState* ignoreDevice(void* handle, const QString& id, const QString& reason);
    void forgetClassification(void* handle);  // drop cached verdict + rate counter
    void noteEvent(void* handle, bool ignored);
    void logInputRates();
    // Bounded delivery receipt: WM_INPUT counts at the window procedure,
    // header/payload read failures and the effective registrations of this
    // process. Logged at startup, on topology changes and when a stream goes
    // silent (plus one follow-up), so a silent in-game stream shows whether
    // WM_INPUT stopped arriving or arrived and was dropped.
    void logDeliveryReceipt(const char* reason);
    QString deviceLabel(void* handle, bool ignored) const;
    void removeDevice(void* handle);
    void reconcileDevices();                  // debounced full-list sync (prune stale handles)
    void failoverOrScheduleDisconnect();
    void finishDisconnect();
    void parseReport(void* handle, DeviceState& st, const unsigned char* data, int len);
    // parseReport stages, in call order. The decoders are pure (static);
    // routeReport owns the active-pad selection/steal side effects.
    static quint32 decodeStickNav(const DeviceState& st, const unsigned char* d, int axisBase, int len);
    void routeReport(void* handle, const DeviceState& st, quint32 s, bool changed,
                     unsigned char reportId, const unsigned char* d, int len);
    void emitEdges(quint32 buttons);
    // Selective Raw HID fallback producer: reached only for ignored HID
    // handles; costs one generation compare + hash lookup unless the device
    // carries a bound raw-HID control or the diagnostics probe is open.
    void rawHidFallbackEvent(void* handle, void* hRawInput);
    void routeRawHidUsageReport(void* handle, const QString& identity,
                                quint64 stateEpoch, const QList<quint32>& current);
    void dropRawHidState(void* handle);   // synthesizes releases for held usages
    // Probe helpers: called only while m_probing (never on the idle fast path).
    void probeIgnoredEvent(void* handle, void* hRawInput);
    void noteProbeButtonChange(const DeviceState& st, quint32 before, quint32 after);
    bool probeWindowStillOpen();   // clears all probe state once the window ends

    std::unique_ptr<RawInputApi> m_api;      // every OS query goes through here
    void* m_hwnd = nullptr;                  // HWND of the message-only window
    QHash<void*, DeviceState> m_devices;     // tracked Sony/DS4 devices by Raw Input handle
    // Negative classification cache: handles already proven to be somebody
    // else's device, mapped to an anonymized endpoint identity for gamepad
    // collections (or a coarse diagnostic label for non-gamepad devices).
    // Keyed strictly by live handle and dropped on every removal, reconcile
    // and arrival — Windows reuses handle VALUES after a replug.
    QHash<void*, QString> m_ignoredHandles;
    // Subset of the ignored handles that are XInput-class (IG_) collections,
    // kept in lockstep with m_ignoredHandles' lifetime rules.
    QHash<void*, QString> m_xinputClass;
    QHash<void*, QString> m_xinputEndpoints;
    QSet<QString> m_loggedIgnored;           // device identities already logged as ignored
    void* m_activeHandle = nullptr;          // device currently driving input (or null)
    QTimer* m_disconnectTimer = nullptr;
    QTimer* m_reconcileTimer = nullptr;
    QTimer* m_topologyTimer = nullptr;
    QTimer* m_rateTimer = nullptr;           // aggregated event-rate log interval
    InputRateMonitor m_rates;
    qint64 m_lastRateSampleMs = 0;
    QElapsedTimer m_clock;
    quint32 m_emittedButtons = 0;            // bitmask InputEngine has seen so far
    bool m_connectedState = false;           // connected(bool) as last emitted
    bool m_sawInput = false;                 // first WM_INPUT diagnostic logged?
    // Delivery counters since the last receipt (GUI thread only).
    quint64 m_wmInputForeground = 0;         // RIM_INPUT
    quint64 m_wmInputSink = 0;               // RIM_INPUTSINK
    quint64 m_headerFailures = 0;            // header read failed or null device
    quint64 m_payloadFailures = 0;
    qint64 m_lastReceiptMs = 0;
    bool m_silenceFollowupPending = false;
    QStringList m_lastHiddenPads;            // last cloak-scan result (change detection)

    // Selective Raw HID fallback state. Eligibility is cached per handle and
    // keyed to SelectiveRawHidFallback::generation() so binding edits and
    // probe transitions re-evaluate without a replug. Pressed usages persist
    // across generations so a button held through a binding edit still
    // releases cleanly.
    int m_rawHidGeneration = -1;
    QHash<void*, int> m_rawHidEligible;      // 1 = bound gamepad HID, -1 = not ours
    QHash<void*, QSet<quint32>> m_rawHidPressed;   // (page<<16)|usage held per handle
    // Bumped by every dropRawHidState(): a rawHidControl handler may tear a
    // device down synchronously, so in-flight delivery must notice that its
    // captured pressed-state snapshot is no longer valid and stop.
    quint64 m_rawHidStateEpoch = 0;

    // Diagnostics probe state; all cleared when the probe window closes.
    bool m_probing = false;                  // one branch on the hot path when idle
    ProbeReadBudget m_probeBudget;           // per-slice read allowance
    QHash<void*, int> m_probeEligible;       // 1 = gamepad-usage HID, -1 = not ours
    QHash<void*, QByteArray> m_probePrev;    // last payload per probed handle
};
