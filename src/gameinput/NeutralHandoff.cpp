#include "gameinput/NeutralHandoff.h"

namespace ModernInput
{

NeutralHandoffDecision decideNeutralHandoff(const NeutralHandoffSample& sample)
{
    NeutralHandoffDecision decision;

    // The overlay input path has to be quiesced first: an action that can still
    // fire after the close is part of the state being handed over.
    if (!sample.overlayActionsQuiesced) {
        decision.neutral = false;
        decision.reason = QStringLiteral("the overlay input path is still live");
        return decision;
    }

    if (sample.heldControls > 0) {
        decision.neutral = false;
        decision.reason = sample.heldSummary.isEmpty()
            ? QStringLiteral("%1 control(s) held").arg(sample.heldControls)
            : QStringLiteral("%1 control(s) held: %2")
                  .arg(sample.heldControls)
                  .arg(sample.heldSummary);
        return decision;
    }

    decision.neutral = true;
    decision.reason = sample.attachedDevices > 0
        ? QStringLiteral("all controls released")
        : QStringLiteral("no controller attached");
    return decision;
}

QString neutralHandoffReceipt(const QString& stage, int durationMs, int polls, const QString& detail)
{
    if (stage == QLatin1String("passed") || stage == QLatin1String("timeout")
        || stage == QLatin1String("released-elsewhere")) {
        QString line = QStringLiteral("%1 duration_ms=%2 polls=%3").arg(stage).arg(durationMs).arg(polls);
        // Only the timeout has a "what was still down" worth printing; on a pass
        // the list would always be empty.
        if (stage == QLatin1String("timeout") && !detail.isEmpty())
            line += QStringLiteral(" held=\"%1\"").arg(detail);
        return line;
    }
    if (detail.isEmpty())
        return stage;
    return QStringLiteral("%1 (%2)").arg(stage, detail);
}

}  // namespace ModernInput
