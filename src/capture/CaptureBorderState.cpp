#include "capture/CaptureBorderState.h"

#include <QStringList>

namespace CaptureBorder {

State derive(const Facts& facts)
{
    // 1. A known pre-22000 build can never suppress the border, no matter what the
    //    rest of the calls report. This is the "never claim hidden on Windows 10" gate.
    if (facts.osBuild != 0 && facts.osBuild < kFirstBuildWithBorderControl)
        return Unsupported;

    // 2. No IGraphicsCaptureSession3 means no border control on this machine.
    if (!facts.sessionInterfaceAvailable)
        return Unsupported;

    // 3. Interface present but the build is unreadable: we cannot prove we are past
    //    22000, so we refuse to claim anything stronger than Unknown.
    if (facts.osBuild == 0)
        return Unknown;

    // 4. Without an answer from the access request there is no evidence either way.
    if (facts.accessStatus == AccessNotRequested)
        return Unknown;

    // 5. Anything other than Allowed (DeniedBySystem, NotDeclaredByApp, DeniedByUser,
    //    UserPromptRequired) means Windows ignores the flag, even though the setter
    //    below still returns S_OK.
    if (facts.accessStatus != AccessAllowed)
        return Denied;

    // 6. Access granted, so the setter and read-back become meaningful. A failure in
    //    either leaves us without proof.
    if (!facts.setterSucceeded || !facts.readBackSucceeded)
        return Unknown;

    // 7. The OS still insists on the border despite a successful put.
    if (facts.readBackBorderRequired)
        return Denied;

    return Hidden;
}

QString stateName(State state)
{
    switch (state) {
    case Unknown:     return QStringLiteral("unknown");
    case Unsupported: return QStringLiteral("unsupported");
    case Denied:      return QStringLiteral("denied");
    case Hidden:      return QStringLiteral("hidden");
    }
    return QStringLiteral("unknown");
}

QString accessStatusName(int status)
{
    switch (status) {
    case AccessNotRequested:       return QStringLiteral("not requested");
    case AccessDeniedBySystem:     return QStringLiteral("DeniedBySystem");
    case AccessNotDeclaredByApp:   return QStringLiteral("NotDeclaredByApp");
    case AccessDeniedByUser:       return QStringLiteral("DeniedByUser");
    case AccessUserPromptRequired: return QStringLiteral("UserPromptRequired");
    case AccessAllowed:            return QStringLiteral("Allowed");
    default:                       return QStringLiteral("status %1").arg(status);
    }
}

QString describe(const Facts& facts, State state)
{
    QStringList parts;
    parts << (facts.osBuild ? QStringLiteral("build %1").arg(facts.osBuild)
                            : QStringLiteral("build unknown"));
    parts << (facts.sessionInterfaceAvailable ? QStringLiteral("session3 available")
                                              : QStringLiteral("session3 missing"));
    parts << QStringLiteral("access %1").arg(accessStatusName(facts.accessStatus));
    if (facts.sessionInterfaceAvailable) {
        parts << (facts.setterSucceeded ? QStringLiteral("setter ok")
                                        : QStringLiteral("setter failed"));
        if (!facts.readBackSucceeded)
            parts << QStringLiteral("read-back failed");
        else
            parts << (facts.readBackBorderRequired ? QStringLiteral("read-back still required")
                                                   : QStringLiteral("read-back suppressed"));
    }
    return QStringLiteral("%1 (%2)").arg(stateName(state), parts.join(QStringLiteral(", ")));
}

} // namespace CaptureBorder
