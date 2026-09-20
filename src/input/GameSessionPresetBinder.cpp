#include "input/GameSessionPresetBinder.h"

#include "input/InputEngine.h"

#include <utility>

GameSessionPresetBinder::GameSessionPresetBinder(InputEngine* engine,
                                                 SessionKeyProvider sessionKey,
                                                 QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_sessionKey(std::move(sessionKey))
{
}

void GameSessionPresetBinder::syncFromSession()
{
    if (!m_engine || !m_sessionKey)
        return;
    // An empty answer is a normal state, not a failure: it clears the game
    // context and the resolver reveals the next winner below it.
    m_engine->setRunningGameKey(m_sessionKey());
}
