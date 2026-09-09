#pragma once

#include <QObject>
#include <QString>
#include <functional>

// Honest reporting of the Windows capture border (S4 / p4-2).
//
// Windows draws a yellow border around anything captured through
// Windows.Graphics.Capture. Suppressing it requires ALL of:
//   * Windows 11 build 22000+ (older builds have no IGraphicsCaptureSession3),
//   * the process holding Borderless capture access
//     (GraphicsCaptureAccess.RequestAccessAsync -> Allowed),
//   * put_IsBorderRequired(false) succeeding AND reading back false,
//   * the flag being set before StartCapture.
//
// What the p4-1 spike actually measured on build 26200 for this unpackaged (Inno
// Setup, no package identity) build: RequestAccessAsync(Borderless) completes with
// Allowed, and put_IsBorderRequired(false) returns S_OK and reads back false both
// before and after StartCapture.
//
// The setter alone still proves nothing: Microsoft documents that IsBorderRequired
// silently succeeds and is ignored when the caller lacks Borderless access, so the
// HRESULT cannot distinguish "suppressed" from "politely ignored". That is why the
// old "capture border hidden" log line was wrong, and why Hidden here requires the
// access grant as well as the read-back — never a setter HRESULT on its own.
namespace CaptureBorder {
Q_NAMESPACE

enum State {
    Unknown,      // the truth could not be established (a call failed, or was never made)
    Unsupported,  // this Windows build cannot suppress the border at all
    Denied,       // supported, but this process has no Borderless capture access
    Hidden,       // access granted, flag set and read back as suppressed
    NotRequested, // this session was started with suppression disabled
};
Q_ENUM_NS(State)

// Windows.Security.Authorization.AppCapabilityAccess.AppCapabilityAccessStatus,
// plus a local sentinel for "we never got as far as asking".
//
// These integers are an ABI contract with Windows, and mingw-w64 publishes no header
// for this enum, so they are NOT guessed: tools/spikes/winrt_enum_probe.exe reads them
// out of C:\WINDOWS\system32\WinMetadata\Windows.Security.winmd via RoGetMetaDataFile
// and IMetaDataImport2. Getting this wrong is not a cosmetic bug — with the ordering
// reversed, a real DeniedBySystem (0) decodes as "Allowed" and the app cheerfully
// reports the border as Hidden. The static_asserts below are the guard rail.
enum AccessStatus {
    AccessNotRequested       = -1,   // local sentinel, not part of the WinRT enum
    AccessDeniedBySystem     = 0,
    AccessNotDeclaredByApp   = 1,
    AccessDeniedByUser       = 2,
    AccessUserPromptRequired = 3,
    AccessAllowed            = 4,
};
Q_ENUM_NS(AccessStatus)

static_assert(AccessDeniedBySystem     == 0, "AppCapabilityAccessStatus::DeniedBySystem is 0");
static_assert(AccessNotDeclaredByApp   == 1, "AppCapabilityAccessStatus::NotDeclaredByApp is 1");
static_assert(AccessDeniedByUser       == 2, "AppCapabilityAccessStatus::DeniedByUser is 2");
static_assert(AccessUserPromptRequired == 3, "AppCapabilityAccessStatus::UserPromptRequired is 3");
static_assert(AccessAllowed            == 4, "AppCapabilityAccessStatus::Allowed is 4");
static_assert(AccessNotRequested < 0, "the not-requested sentinel must not collide with a real status");

// First Windows build shipping IGraphicsCaptureSession3.IsBorderRequired.
inline constexpr unsigned long kFirstBuildWithBorderControl = 22000;

// Everything derive() is allowed to look at. The capture worker fills this in from
// real calls; tests fill it in by hand.
struct Facts {
    bool suppressionRequested = true;         // immutable preference for this session
    unsigned long osBuild = 0;                // 0 = could not be read
    bool sessionInterfaceAvailable = false;   // QI(IGraphicsCaptureSession3) succeeded
    int  accessStatus = AccessNotRequested;   // AppCapabilityAccessStatus
    bool setterSucceeded = false;             // put_IsBorderRequired(false) returned S_OK
    bool readBackSucceeded = false;           // get_IsBorderRequired returned S_OK
    bool readBackBorderRequired = true;       // the value it returned (true = border stays)
};

// Snapshot at session dispatch. Applying it can never consult changed settings;
// disabled sessions do not even invoke the Windows probe/access/setter callback.
class SessionPolicy {
public:
    explicit SessionPolicy(bool requested = true) : m_requested(requested) {}
    bool requested() const { return m_requested; }
    Facts collect(const std::function<Facts()>& requestSuppression) const;
private:
    bool m_requested;
};

// Pure, side-effect free, and the only place a Hidden verdict may be produced.
State derive(const Facts& facts);

QString stateName(State state);
QString accessStatusName(int status);

// One diagnostic line for the log and the capture status, e.g.
// "denied (build 26200, access UserPromptRequired, setter ok, read-back suppressed)".
// Deliberately untranslated: p4-3 owns the user-facing wording.
QString describe(const Facts& facts, State state);

} // namespace CaptureBorder

Q_DECLARE_METATYPE(CaptureBorder::SessionPolicy)
