#pragma once

#include "input/MappingPresetModel.h"

#include <QObject>
#include <QString>

#include <functional>

class InputEngine;

// The one seam that pushes the app's game session into the mapping game context
// (cpo-p07, docs/mapping-presets.md section 7c). It is transport only, and
// deliberately so:
//
// - The session decides the key. The provider answers with the session's
//   canonical executable key (CurrentGameService), so the binder never inspects
//   the foreground window and never manufactures an identity. Opening, using or
//   closing the overlay therefore cannot retarget the mappings: the overlay is
//   an excluded process, not a game-session transition.
// - It resolves nothing itself. InputEngine::setRunningGameKey() compares the
//   key and forwards a real change to the cpo-p05 safe switch boundary, which
//   invalidates the old generation and gates controls that are still held. A
//   push that resolves to the same key is one string compare.
// - The key is read when the push happens, not when the session changed, so a
//   game row whose executable path was only just learned is already visible.
class GameSessionPresetBinder : public QObject
{
    Q_OBJECT

public:
    // Returns the canonical game key of the current session game, or empty when
    // no game is in session.
    using SessionKeyProvider = std::function<QString()>;

    GameSessionPresetBinder(InputEngine* engine, SessionKeyProvider sessionKey,
                            QObject* parent = nullptr);

    // The COMPLETE session -> mapping-context wiring, in one place so the app and
    // the tests drive the same connections: `sessionSignals` is AppController in
    // production and a stub carrying the same two signals in tests.
    //
    // Both signals matter, and both consumers listen to both:
    // - currentGameChanged: the session moved to another game (or to none);
    // - gamesChanged: the SAME session game just learned or changed its stored
    //   executable identity. CurrentGameService::update() reports NO state change
    //   for that (the game id and its capture state did not move), so it only
    //   surfaces through the library signal - rememberGameExecutable() from a
    //   foreground poll, a capture commit, or a metadata repair.
    // Without the second pair, the engine and the Settings game row keep the old
    // (or empty) key until some unrelated later transition, and a per-game
    // assignment stays inert in the meantime. Repeated sync is a string compare
    // plus one rebuild, so the overlap between the two signals is free.
    template <typename SessionSource>
    static void wireGameSessionContext(SessionSource* sessionSignals,
                                       GameSessionPresetBinder* binder,
                                       MappingPresetModel* model)
    {
        QObject::connect(sessionSignals, &SessionSource::currentGameChanged,
                         binder, &GameSessionPresetBinder::syncFromSession);
        QObject::connect(sessionSignals, &SessionSource::gamesChanged,
                         binder, &GameSessionPresetBinder::syncFromSession);
        QObject::connect(sessionSignals, &SessionSource::currentGameChanged,
                         model, &MappingPresetModel::refreshGameTarget);
        QObject::connect(sessionSignals, &SessionSource::gamesChanged,
                         model, &MappingPresetModel::refreshGameTarget);
    }

public slots:
    // Push the session's current key into the engine. Cheap and idempotent; the
    // wiring above calls it on every session signal that can move the context.
    void syncFromSession();

private:
    InputEngine* m_engine = nullptr;
    SessionKeyProvider m_sessionKey;
};
