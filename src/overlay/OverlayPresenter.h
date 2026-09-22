#pragma once

#include <QRect>
#include <QString>

#include <memory>

class QWindow;

// The single place that makes the overlay window visible, positioned and
// topmost WITHOUT ever activating it (cpo-o02). Every production show path
// goes through here, so the no-activation guarantee cannot be re-derived —
// or forgotten — per call site.
//
// Two independent protections are applied together, deliberately:
//   * WS_EX_NOACTIVATE on the *current* native handle, so Windows refuses to
//     activate the window on its own (it otherwise promotes the next topmost
//     window when the foreground one hides or minimizes);
//   * SWP_NOACTIVATE on every native positioning / z-order call.
// Neither replaces the other: the ex-style can be lost when Qt rebuilds the
// native window, and a plain SetWindowPos without SWP_NOACTIVATE activates.
//
// cpo-o06b adds a second, explicitly requested mode: makeActivatable() clears
// WS_EX_NOACTIVATE so a foreground request can succeed. It is never part of
// present() — presentation always starts from the non-activating path, and an
// overlay whose foreground request is denied simply stays where that path left
// it. SWP_NOACTIVATE stays on every positioning call in BOTH modes: the only
// thing that may move the foreground is the caller's explicit request.
namespace OverlayWin32
{
// Mirrored from <windows.h> so this unit — and its tests — stay free of it.
// OverlayPresenter.cpp static_asserts each value against the real macro.
constexpr unsigned long kExNoActivate = 0x08000000uL;  // WS_EX_NOACTIVATE
constexpr unsigned kSwpNoSize = 0x0001u;
constexpr unsigned kSwpNoMove = 0x0002u;
constexpr unsigned kSwpNoZOrder = 0x0004u;
constexpr unsigned kSwpNoActivate = 0x0010u;
constexpr unsigned kSwpFrameChanged = 0x0020u;

// HWND_TOPMOST. Not constexpr: a reinterpret_cast never is.
inline void* topmost()
{
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}
}  // namespace OverlayWin32

// Test seam. The production implementation drives a real QWindow plus Win32;
// tests drive a recording fake, including one that swaps the native handle
// mid-call the way Qt does when it rebuilds the window.
class OverlayWindowApi
{
public:
    virtual ~OverlayWindowApi() = default;

    // Current native handle, creating it if the window has none yet.
    virtual void* nativeHandle() = 0;
    virtual unsigned long extendedStyle(void* hwnd) = 0;
    virtual void setExtendedStyle(void* hwnd, unsigned long style) = 0;
    virtual void setWindowPos(void* hwnd, void* insertAfter, unsigned flags) = 0;
    // Qt-level: the scene graph has to know about geometry and visibility,
    // so these two do not bypass QWindow.
    virtual void setGeometry(const QRect& rect) = 0;
    virtual void showWindow() = 0;
    virtual void* foregroundWindow() = 0;
};

// What one presentation actually did, so the log can state it instead of
// asserting it in a comment.
struct OverlayPresentReport
{
    void* handle = nullptr;
    bool recreated = false;      // Qt handed us a different HWND than last time
    bool styleApplied = false;   // the activation ex-style had to be (re)written
    bool activatable = false;    // the mode this report was produced under
    void* foregroundBefore = nullptr;
    void* foregroundAfter = nullptr;

    bool foregroundPreserved() const { return foregroundBefore == foregroundAfter; }
    QString toLogString() const;
};

class OverlayPresenter
{
public:
    explicit OverlayPresenter(std::unique_ptr<OverlayWindowApi> api);
    ~OverlayPresenter();

    // Move the window to `geometry` (when valid), show it and pin it topmost
    // — all without activation. Safe to call repeatedly.
    OverlayPresentReport present(const QRect& geometry);

    // Re-apply the guarantee to whatever handle exists now, without touching
    // geometry or visibility: used after a screen change, where Qt may have
    // rebuilt the native window under a visible overlay.
    OverlayPresentReport reassert();

    // cpo-o06b: switch the already-presented window to activatable — clears
    // WS_EX_NOACTIVATE so an explicit foreground request can succeed. It is a
    // deliberate second step AFTER present(), never part of it: presentation
    // keeps its never-activate guarantee, and a window that was shown without
    // activation stays that way if this is never called (or if the foreground
    // request that follows is denied).
    //
    // Geometry, visibility and z-order are untouched here; the mode sticks, so
    // a later reassert()/present() — a screen change, the game moving monitors
    // — keeps the overlay activatable instead of silently reverting it.
    OverlayPresentReport makeActivatable();

    // Back to the non-activating default, applied by the next present()/
    // reassert(). Called at the start of every open so each show begins from
    // the proven path rather than inheriting the previous session's mode.
    void resetActivationPolicy() { m_activatable = false; }

    bool isActivatable() const { return m_activatable; }

private:
    // Writes WS_EX_NOACTIVATE or clears it, whichever the current mode wants.
    bool ensureActivationStyle(void* hwnd, OverlayPresentReport& report);

    std::unique_ptr<OverlayWindowApi> m_api;
    void* m_styledHandle = nullptr;
    bool m_activatable = false;
};

// Production seam over a real QWindow. Declared here so the overlay test can
// exercise the same adapter the app ships instead of a look-alike.
std::unique_ptr<OverlayWindowApi> makeQWindowOverlayApi(QWindow* window);
