#pragma once

// Shared by the OverlayManager*.cpp translation units only; not part of the
// overlay manager's interface.

#include "overlay/OverlayLifetimePolicy.h"
#include "overlay/OverlayManager.h"

#include <QString>

#include <windows.h>

namespace OverlayManagerInternal {
// The two acquisition phases, spelled once: ForegroundAcquirer logs them and
// InputDiagnostics records them, so the completion handler must match exactly.
inline const QString kShowPhase = QStringLiteral("overlay show");
inline const QString kHidePhase = QStringLiteral("overlay hide");

// 0 for a missing or already-destroyed handle, so "no remembered game" and
// "remembered game is gone" both fail closed instead of matching anything.
inline unsigned long processIdOfWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}
} // namespace OverlayManagerInternal

// The four effects a lifetime decision can have, all performed through the one
// presenter that owns the never-activate guarantee — no second show path.
class OverlayManager::LifetimeActions final : public OverlayLifetime::Actions
{
public:
    explicit LifetimeActions(OverlayManager& manager)
        : m_manager(manager)
    {
    }

    void rebindGameWindow(void* newWindow) override { m_manager.rebindGameWindow(newWindow); }
    void reassertOverlay() override { m_manager.reassertOverlay(); }
    void repositionOverlay() override { m_manager.repositionOverlay(); }
    void hideOverlay() override { m_manager.hideInternal(); }

private:
    OverlayManager& m_manager;
};
