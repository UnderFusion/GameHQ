#pragma once
#include <QObject>
#include <QString>

#include <memory>

class QQmlApplicationEngine;
class QQuickWindow;
class QScreen;
class OverlayPresenter;

// In-game overlay window lifecycle (docs/overlay.md): lazy-loads
// OverlayWindow.qml, shows it frameless/topmost over the active app,
// remembers the foreground game without taking or restoring OS focus.
// No injection — borderless/windowed fullscreen games only (MVP).
class OverlayManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool visible READ isVisible NOTIFY visibleChanged)
    // False while open: the non-activating overlay leaves input with the game.
    // True while closed only suppresses the existing input warning.
    Q_PROPERTY(bool foregroundAcquired READ foregroundAcquired NOTIFY foregroundAcquiredChanged)

public:
    explicit OverlayManager(QQmlApplicationEngine* engine, QObject* parent = nullptr);
    ~OverlayManager() override;

    bool isVisible() const;
    bool foregroundAcquired() const { return m_foregroundAcquired; }

    Q_INVOKABLE void toggle();
    Q_INVOKABLE void show();
    Q_INVOKABLE void hide();

    // Desktop-window summon (hold PS): the overlay gets out of the way but
    // must NOT hand focus back to the game — the desktop window is about to
    // take it. Returns the window the overlay had remembered (null when it
    // was not visible) so the caller can restore focus there later.
    void* hideForDesktopHandoff();

    // Called from the WinEvent hook callback (see .cpp) whenever the OS
    // foreground window changes to something other than the overlay itself
    // while the overlay is visible — Win key (Start menu), Alt-Tab, the task
    // switcher, or a click on another app all land here. Public so the free
    // function callback can reach it; not meant for QML/general use.
    void onForegroundWindowChanged(void* newForeground);

signals:
    void aboutToShow();
    void visibleChanged();
    void foregroundAcquiredChanged();

private:
    bool ensureLoaded();
    void hideInternal();
    void startShowProbe();
    void probeTick();

    // cpo-o03: the lifetime decision is pure (overlay/OverlayLifetimePolicy.h);
    // this class only resolves Win32 facts for it and performs the effects
    // through the existing presenter.
    class LifetimeActions;
    // The screen the overlay should cover for `gameWindow`: the game's monitor
    // when that handle still exists, the primary screen otherwise. A stale
    // HWND can therefore never drive overlay geometry.
    QScreen* targetScreenForGameWindow(void* gameWindow) const;
    bool gameWindowMovedToAnotherMonitor(void* gameWindow) const;
    void rebindGameWindow(void* newWindow);
    void reassertOverlay();
    void repositionOverlay();

    QQmlApplicationEngine* m_engine;
    QQuickWindow* m_window = nullptr;
    // The one production path that makes the overlay visible, positioned and
    // topmost; it owns the never-activate guarantee (docs/overlay.md).
    std::unique_ptr<OverlayPresenter> m_presenter;
    std::unique_ptr<LifetimeActions> m_lifetimeActions;
    void* m_previousForeground = nullptr;   // HWND of the game/app under us
    // Process id of the window above. It is the continuity evidence that keeps
    // the overlay open when a game replaces its own window, and the reason a
    // destroyed HWND is never remembered: a pid outlives a handle.
    unsigned long m_previousForegroundPid = 0;
    void* m_focusHook = nullptr;            // HWINEVENTHOOK, opaque here to avoid <windows.h> in the header
    bool m_foregroundAcquired = true;

    // Diagnostic probe: for a few seconds after show() the game window's
    // state is sampled and every change is logged, so a game that hides or
    // minimizes itself in reaction to the overlay leaves evidence in the log.
    class QTimer* m_probeTimer = nullptr;
    int m_probeElapsedMs = 0;
    QString m_probeLastState;
};
