#include "input/OverlayInputPolicy.h"

#include "input/ContextOverrideCatalog.h"

#include <algorithm>

namespace OverlayInput {

ActionCatalog::Scope primaryScope(const Context& context)
{
    if (context.playbackActive)
        return ActionCatalog::Scope::Playback;
    if (context.overlayVisible)
        return ActionCatalog::Scope::Overlay;
    if (context.desktopFocused)
        return ActionCatalog::Scope::Desktop;
    return ActionCatalog::Scope::Global;
}

ActionCatalog::Scope fallbackScope(const Context& context)
{
    if (!context.playbackActive)
        return ActionCatalog::Scope::Global;
    return context.overlayVisible ? ActionCatalog::Scope::Overlay
                                  : ActionCatalog::Scope::Desktop;
}

Selection select(const QVector<Candidate>& candidates, const GestureSpec& gesture,
                 ActionCatalog::Scope primary, ActionCatalog::Scope fallback)
{
    Selection selection;
    QVector<int> primaryIndices;
    QVector<int> fallbackIndices;
    bool primaryOwnsTrigger = false;
    for (int i = 0; i < candidates.size(); ++i) {
        const Candidate& candidate = candidates.at(i);
        // Context priority belongs to the physical trigger, not to each
        // gesture independently. Otherwise a Playback press on Cross can fire
        // first, then fall through to Desktop's tap on release (and its hold
        // after one second), producing two actions from one press cycle.
        if (candidate.scope == primary && candidate.scope != ActionCatalog::Scope::Global)
            primaryOwnsTrigger = true;

        // Kind AND tap count: "tap" alone would let a double-tap binding answer
        // a single tap. Hold durations are deliberately not compared — the
        // runtime decides which hold is due from elapsed time.
        if (candidate.gesture.kind != gesture.kind
            || candidate.gesture.tapCount != gesture.tapCount)
            continue;

        if (candidate.scope == ActionCatalog::Scope::Global)
            selection.globalIndices.append(i);
        else if (candidate.scope == primary)
            primaryIndices.append(i);
        else if (candidate.scope == fallback)
            fallbackIndices.append(i);
    }
    selection.contextualIndices = primaryOwnsTrigger ? primaryIndices : fallbackIndices;

    // Global bindings normally fire alongside the contextual ones, so a Guide
    // press still toggles the overlay while a clip is focused. Drop only the
    // pairs ContextOverrideCatalog declares as deliberate substitutions —
    // never a blanket "contextual shadows Global" rule, which would hide
    // user-created overlaps the binding editor is supposed to surface.
    const QVector<int> globals = selection.globalIndices;
    selection.globalIndices.clear();
    for (int index : globals) {
        const QString& globalAction = candidates.at(index).actionId;
        const bool shadowed = std::any_of(
            selection.contextualIndices.cbegin(), selection.contextualIndices.cend(),
            [&](int contextualIndex) {
                return ContextOverrideCatalog::shadows(candidates.at(contextualIndex).actionId,
                                                       globalAction, gesture);
            });
        if (!shadowed)
            selection.globalIndices.append(index);
    }
    return selection;
}

Selection select(const QVector<Candidate>& candidates, const GestureSpec& gesture,
                 const Context& context)
{
    return select(candidates, gesture, primaryScope(context), fallbackScope(context));
}

QStringList deliveredActions(const QVector<Candidate>& candidates, const GestureSpec& gesture,
                             const Context& context)
{
    const Selection selection = select(candidates, gesture, context);
    QStringList actions;
    for (int index : selection.globalIndices)
        actions.append(candidates.at(index).actionId);
    for (int index : selection.contextualIndices)
        actions.append(candidates.at(index).actionId);
    return actions;
}

} // namespace OverlayInput
