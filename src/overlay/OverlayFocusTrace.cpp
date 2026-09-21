#include "overlay/OverlayFocusTrace.h"

namespace OverlayFocus
{

QString formatHandle(const void* handle)
{
    if (!handle)
        return QStringLiteral("none");
    return QStringLiteral("0x%1").arg(
        QString::number(reinterpret_cast<quintptr>(handle), 16));
}

namespace
{
QString orUnavailable(const QString& value)
{
    return value.isEmpty() ? QStringLiteral("unavailable") : value;
}
}  // namespace

QString WindowFacts::toLogString() const
{
    if (!handle)
        return QStringLiteral("none");
    if (!exists)
        return QStringLiteral("%1 destroyed").arg(formatHandle(handle));
    return QStringLiteral("%1 visible=%2 iconic=%3 pid=%4 rect=%5,%6-%7,%8")
        .arg(formatHandle(handle))
        .arg(visible ? 1 : 0)
        .arg(iconic ? 1 : 0)
        .arg(pid)
        .arg(left)
        .arg(top)
        .arg(right)
        .arg(bottom);
}

QString ShowTrace::toLogString() const
{
    // One line, fixed field order: a report pasted from two different sessions
    // has to be diffable.
    return QStringLiteral(
               "game=[%1] overlay=[%2] fg before=%3 after-present=%4 "
               "after-activation=%5 activation-requested=%6 overlay-foreground=%7 "
               "qt-active=%8 qt/win32-disagree=%9 game-kept-foreground=%10 "
               "provider=%11 profile=%12 gameinput-focus-policy=%13 isolation=%14")
        .arg(game.toLogString(),
             overlay.toLogString(),
             formatHandle(foregroundBefore),
             formatHandle(foregroundAfterPresent),
             formatHandle(foregroundAfterActivation),
             activationRequested ? QStringLiteral("yes") : QStringLiteral("no"),
             overlayOwnsForeground() ? QStringLiteral("yes") : QStringLiteral("no"),
             overlayActiveQt ? QStringLiteral("yes") : QStringLiteral("no"),
             qtWin32Disagree() ? QStringLiteral("yes") : QStringLiteral("no"))
        // Split deliberately: QString::arg() takes at most nine strings, and
        // the multi-arg form fills the lowest-numbered markers left, so %10
        // onwards belong to a second call.
        .arg(foregroundPreserved() ? QStringLiteral("yes") : QStringLiteral("no"),
             orUnavailable(controllerProvider),
             orUnavailable(controllerProfile),
             orUnavailable(gameInputFocusPolicy),
             isolationEvidence());
}

QString HideTrace::toLogString() const
{
    return QStringLiteral(
               "fg before=%1 after=%2 restore-target=%3 restore-requested=%4 "
               "restored=%5 neutral-handoff=%6 provider before=%7 after=%8")
        .arg(formatHandle(foregroundBefore),
             formatHandle(foregroundAfter),
             formatHandle(restoreTarget),
             restoreRequested ? QStringLiteral("yes") : QStringLiteral("no"),
             restored ? QStringLiteral("yes") : QStringLiteral("no"),
             orUnavailable(neutralHandoff),
             orUnavailable(providerBefore),
             orUnavailable(providerAfter));
}

}  // namespace OverlayFocus
