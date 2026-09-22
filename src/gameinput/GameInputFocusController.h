#pragma once

#include "gameinput/GameInputFocusPolicy.h"

#include <QObject>
#include <QString>

namespace ModernInput
{

class IGameInputApi;

// cpo-o06c: the one owner of the process-wide GameInput focus policy.
//
// GameInput's focus policy is a single process-wide setting, so exactly one
// object in GameHQ is allowed to call IGameInputApi::applyFocusPolicy(): this
// one. Interactive surfaces (today: the overlay) only REQUEST a transition; the
// controller decides whether one happens, applies it through that single seam,
// records it for the diagnostics export and announces it.
//
// The scoping rule this class exists to enforce, rather than leaving it to two
// call sites that could disagree:
//
//   no runtime attached                     -> background, requests refused
//   runtime attached, overlay not shown     -> background
//   overlay verified interactive foreground -> exclusive-foreground  (request)
//   close / focus lost / game gone / stop   -> background            (release)
//
// It is not a state that can be left on: attach() always starts a fresh session
// in the background policy, and detach() restores it while the runtime is still
// alive to receive the call.
class GameInputFocusController final : public QObject, public GameInputFocusRequestSink
{
    Q_OBJECT

public:
    explicit GameInputFocusController(QObject* parent = nullptr);
    ~GameInputFocusController() override;

    // Runtime session lifetime. attach() puts the session into the background
    // policy — the start-time contract: the policy has to be in force before the
    // first callback registration — and detach() returns to it before the runtime
    // goes away, so an unloaded, failed or restarted runtime can never inherit
    // the exclusive state an interactive overlay asked for.
    void attach(IGameInputApi* api);
    void detach();

    bool requestExclusiveForeground(const QString& reason) override;
    void restoreBackground(const QString& reason) override;

    GameInputFocusMode focusMode() const override { return m_mode; }
    bool exclusiveForegroundActive() const override
    {
        return m_mode == GameInputFocusMode::ExclusiveForeground;
    }
    QString focusModeName() const override { return gameInputFocusModeName(m_mode); }
    int focusTransitionCount() const override { return m_transitionCount; }
    bool policyAttached() const override { return m_api != nullptr; }

    QString lastTransitionReason() const { return m_lastReason; }
    // Requests that changed nothing because the policy was already in force, and
    // requests refused because no runtime was attached. Diagnostics only: they
    // prove a duplicate request sequence stayed flat instead of re-issuing flags.
    int duplicateRequestCount() const { return m_duplicateRequests; }
    int refusedRequestCount() const { return m_refusedRequests; }

signals:
    // Emitted once per application of the policy, never per request: a duplicate
    // request while the policy is already in force changes nothing and stays
    // silent. `mode` is gameInputFocusModeName().
    void focusPolicyTransitioned(const QString& mode, const QString& reason);

private:
    void apply(GameInputFocusMode mode, const QString& reason);

    IGameInputApi* m_api = nullptr;
    GameInputFocusMode m_mode = GameInputFocusMode::Background;
    int m_transitionCount = 0;
    int m_duplicateRequests = 0;
    int m_refusedRequests = 0;
    QString m_lastReason;
};

}  // namespace ModernInput
