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

QString GameInputPolicyFacts::toLogString() const
{
    return QStringLiteral("mode=%1 request=%2 reason=\"%3\" transitions=%4")
        .arg(orUnavailable(mode), orUnavailable(request), orUnavailable(reason))
        .arg(transitions);
}

GameInputPolicyDecision decideGameInputPolicy(const ShowTrace& trace)
{
    GameInputPolicyDecision decision;
    // Clause order mirrors interactiveForegroundTruth(); tst_overlayfocustrace
    // pins that the two cannot disagree.
    if (!trace.activationRequested) {
        decision.reason = QStringLiteral("no foreground request was made");
        return decision;
    }
    if (!trace.acquisitionSucceeded) {
        decision.reason = QStringLiteral("foreground was not acquired");
        return decision;
    }
    if (!trace.gameAfterAcquisition.exists) {
        decision.reason = QStringLiteral("the game window is gone");
        return decision;
    }
    if (trace.gameAfterAcquisition.iconic) {
        decision.reason = QStringLiteral("the game minimized itself");
        return decision;
    }
    if (!trace.gameAfterAcquisition.visible) {
        decision.reason = QStringLiteral("the game window is no longer visible");
        return decision;
    }
    if (!trace.overlayAfterAcquisition.exists || !trace.overlayAfterAcquisition.visible) {
        decision.reason = QStringLiteral("the overlay window is not visible");
        return decision;
    }
    if (!trace.overlayOwnsForeground()) {
        decision.reason = QStringLiteral("the overlay does not own the foreground");
        return decision;
    }
    decision.request = true;
    decision.reason = QStringLiteral("overlay holds the verified interactive foreground");
    return decision;
}

QString gameInputRestoreReason(bool gameAlive, bool gameIconic,
                               bool overlayOwnedForeground, bool desktopHandoff)
{
    // Most specific fact first: the report should say what actually ended the
    // interactive state, not the generic "the overlay closed" whenever it can be
    // more precise than that.
    if (!gameAlive)
        return QStringLiteral("the game window is gone");
    if (gameIconic)
        return QStringLiteral("the game was minimized");
    if (!overlayOwnedForeground)
        return QStringLiteral("the overlay lost the foreground");
    if (desktopHandoff)
        return QStringLiteral("desktop handoff");
    return QStringLiteral("the overlay closed");
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
                       "gameinput-focus-policy=%4 %5")
            .arg(foregroundPreserved() ? QStringLiteral("yes") : QStringLiteral("no"),
                 orUnavailable(controllerProvider),
                 orUnavailable(controllerProfile),
                 orUnavailable(gameInputFocusPolicy),
                 // cpo-o06f: the classification carries its own `isolation=`
                 // label (gameinput/IsolationCapability.h), so this formatter
                 // never adds a second one; a record with none says why.
                 isolation.isEmpty()
                     ? QStringLiteral("isolation=unclassified (this open recorded no classification)")
                     : isolation);

    // cpo-o06c: the policy request this open made (or refused), and why. Still no
    // isolation claim: the request is what GameHQ asked for, not proof that
    // another process lost the pad.
    const QString policy =
        QStringLiteral(" gameinput-policy=[%1]").arg(gameInputPolicy.toLogString());

    return windows + acquisition + controller + policy;
}

QString HideTrace::toLogString() const
{
    return QStringLiteral(
               "fg before=%1 after=%2 restore-target=%3 restore-requested=%4 "
               "restored=%5 neutral-handoff=%6 provider before=%7 after=%8 "
               "gameinput-policy=[%9]")
        .arg(formatHandle(foregroundBefore),
             formatHandle(foregroundAfter),
             formatHandle(restoreTarget),
             restoreRequested ? QStringLiteral("yes") : QStringLiteral("no"),
             restored ? QStringLiteral("yes") : QStringLiteral("no"),
             orUnavailable(neutralHandoff),
             orUnavailable(providerBefore),
             orUnavailable(providerAfter),
             gameInputPolicy.toLogString());
}

}  // namespace OverlayFocus
