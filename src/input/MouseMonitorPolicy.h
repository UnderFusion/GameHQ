#pragma once
#include <QString>

// Decides whether the global mouse hook (MouseHookDevice) should be
// installed. Kept as a free function so the decision is testable without a
// live InputEngine (tst_mousehooklazy): the hook exists only while a mouse
// binding can consume its events, or while the user is capturing one — a
// first mouse binding must be observable before any binding exists yet.
namespace MouseMonitorPolicy {

inline bool needed(bool hasMouseBindings, const QString& editorDeviceGroup,
                   bool captureActive, const QString& editorCaptureStep)
{
    if (hasMouseBindings)
        return true;
    return editorDeviceGroup == QLatin1String("mouse")
        && (captureActive || editorCaptureStep != QLatin1String("idle"));
}

} // namespace MouseMonitorPolicy
