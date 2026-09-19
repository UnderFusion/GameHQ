#pragma once
#include <QObject>
#include <QString>

class QQmlApplicationEngine;
class QQuickWindow;

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
    void applyNoActivateStyle();
    void startShowProbe();
    void probeTick();

    QQmlApplicationEngine* m_engine;
    QQuickWindow* m_window = nullptr;
    void* m_previousForeground = nullptr;   // HWND of the game/app under us
    void* m_focusHook = nullptr;            // HWINEVENTHOOK, opaque here to avoid <windows.h> in the header
    bool m_foregroundAcquired = true;

    // Diagnostic probe: for a few seconds after show() the game window's
    // state is sampled and every change is logged, so a game that hides or
    // minimizes itself in reaction to the overlay leaves evidence in the log.
    class QTimer* m_probeTimer = nullptr;
    int m_probeElapsedMs = 0;
    QString m_probeLastState;
};
