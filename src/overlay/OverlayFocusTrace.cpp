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
    // has to be diffable. Assembled from three pieces because QString::arg()
    // takes at most nine strings at a time; the pieces are concatenated, never
    // chained through arg(), which would silently drop fields once the first
    // call has consumed every placeholder.
    const QString windows = QStringLiteral(
                                "game=[%1] overlay=[%2] fg before=%3 after-present=%4 "
                                "after-activation=%5 activation-requested=%6 "
                                "overlay-foreground=%7 qt-active=%8 qt/win32-disagree=%9")
                                .arg(game.toLogString(),
                                     overlay.toLogString(),
                                     formatHandle(foregroundBefore),
                                     formatHandle(foregroundAfterPresent),
                                     formatHandle(foregroundAfterActivation),
                                     activationRequested ? QStringLiteral("yes")
                                                         : QStringLiteral("no"),
                                     overlayOwnsForeground() ? QStringLiteral("yes")
                                                             : QStringLiteral("no"),
                                     overlayActiveQt ? QStringLiteral("yes")
                                                     : QStringLiteral("no"),
                                     qtWin32Disagree() ? QStringLiteral("yes")
                                                       : QStringLiteral("no"));

    // cpo-o06b: what the explicit foreground request actually achieved, with
    // both windows re-sampled afterwards so the verdict below is derived from
    // observation rather than from the API having returned.
    const QString acquisition =
        QStringLiteral(" acquisition=%1 attempts=%2 game-after=[%3] overlay-after=[%4] "
                       "interactive-foreground=%5 lifetime-accepted-overlay-foreground=%6")
            .arg(acquisitionSucceeded ? QStringLiteral("acquired")
                                      : QStringLiteral("not-acquired"),
                 QString::number(acquisitionAttempts),
                 gameAfterAcquisition.toLogString(),
                 overlayAfterAcquisition.toLogString(),
                 interactiveForegroundTruth() ? QStringLiteral("yes") : QStringLiteral("no"),
                 lifetimeAcceptedOverlayForeground ? QStringLiteral("yes")
                                                   : QStringLiteral("no"));

    const QString controller =
        QStringLiteral(" game-kept-foreground=%1 provider=%2 profile=%3 "
                       "gameinput-focus-policy=%4 isolation=%5")
            .arg(foregroundPreserved() ? QStringLiteral("yes") : QStringLiteral("no"),
                 orUnavailable(controllerProvider),
                 orUnavailable(controllerProfile),
                 orUnavailable(gameInputFocusPolicy),
                 isolationEvidence());

    return windows + acquisition + controller;
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
