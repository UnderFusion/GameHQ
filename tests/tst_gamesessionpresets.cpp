// The game session IS the mapping game context (cpo-p07).
//
// The unit suites already prove the engine seam (setRunningGameKey switches the
// winner) and the resolver's precedence. What this suite covers is the wiring
// ABOVE them, end to end and with the shipped classes:
//
//   CurrentGameService  ->  GameSessionPresetBinder  ->  InputEngine
//
// so that the two invariants of this leaf are held by construction rather than
// by inspection:
//   - opening/using the overlay never changes, clears or retargets the active
//     game context (the session survives an excluded foreground process and a
//     game that is still running);
//   - game exit removes only the game-context winner and reveals the next one
//     through the existing resolver - no table replacement, no parallel switch.
//
// The session is driven with CurrentGameService::update() (the same call the app
// makes from the foreground detector), never through the real foreground window:
// the fixture must not depend on what happens to be focused on the desktop. The
// "still running" case uses THIS test binary's own path as the game executable,
// so the service's running-process fallback sees a live process exactly like a
// launched game.

#include "input/InputEngine.h"
#include "input/GameSessionPresetBinder.h"
#include "input/XInputDevice.h"
#include "input/BindingRuntime.h"
#include "input/BindingResolver.h"
#include "input/MappingAssignmentResolver.h"
#include "input/MappingPresetModel.h"
#include "config/ConfigManager.h"
#include "core/GameIdentity.h"
#include "storage/CaptureDatabase.h"
#include "ui/CurrentGameService.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace ModernInput;

namespace {

const QString kController = QStringLiteral("controller");
const QString kKeyboard = QStringLiteral("keyboard");

MappingPresetRow presetRow(const QString& actionId, const QString& trigger,
                           const QString& activation = QStringLiteral("press"))
{
    MappingPresetRow row;
    row.actionId = actionId;
    row.slot = 1;
    row.triggerCode = trigger;
    row.activation = activation;
    row.tapCount = 1;
    return row;
}

} // namespace

// The production XInput edge publisher without the system DLL.
class SessionSyntheticXInput final : public XInputDevice
{
public:
    using XInputDevice::XInputDevice;
    void buttons(quint32 mask) { setSlotState(0, mask, true); }
};

// AppController, reduced to the two session signals the cpo-p07 wiring consumes
// (nothing else of it is reachable from a unit suite). publish() takes the two
// facts update() reports, so the harness announces exactly what the app would:
// a real transition is currentGameChanged, a metadata-only change - the SAME
// game row learning or changing its executable - is gamesChanged.
class SessionSignalsStub : public QObject
{
    Q_OBJECT

public:
    void publish(bool transitioned, bool metadataChanged)
    {
        if (transitioned)
            emit currentGameChanged();
        if (metadataChanged)
            emit gamesChanged();
    }

signals:
    void currentGameChanged();
    void gamesChanged();
};

class GameSessionPresetTest : public QObject
{
    Q_OBJECT

private:
    BindingRuntime& rt() { return *engine->m_runtime; }

    // A game executable that EXISTS on disk but is not running, so the session's
    // running-process fallback cannot hold the session once the foreground leaves
    // it - which is what an installed but closed game looks like.
    QString offlineGameExecutable(const QString& leaf)
    {
        const QString path = dir->filePath(leaf + QStringLiteral(".exe"));
        if (!QFileInfo::exists(path)) {
            QFile fixture(path);
            fixture.open(QIODevice::WriteOnly);
            fixture.write("game executable fixture\n");
            fixture.close();
        }
        return path;
    }

    // A game the session can be in. CurrentGameService reads the SAME game rows
    // the overlay does, and those are the captured ones (CaptureQueries::listGames
    // joins captures), so a fixture game needs exactly one capture - which is also
    // what creates the games row with its executable path.
    int capturedGame(const QString& name, const QString& executablePath)
    {
        const QString capture = dir->filePath(name + QStringLiteral(".png"));
        if (!QFileInfo::exists(capture)) {
            QFile fixture(capture);
            fixture.open(QIODevice::WriteOnly);
            fixture.write("capture fixture\n");
            fixture.close();
        }
        return db->insertCapture(capture, QStringLiteral("screenshot"), name,
                                 QStringLiteral("2026-09-20T12:00:00Z"), QStringLiteral("test"),
                                 executablePath);
    }

    QString makePreset(const QString& group, const QString& name,
                       const QVector<MappingPresetRow>& rows)
    {
        return db->createMappingPreset(group, name, rows);
    }

    void assignGroupDefault(const QString& group, const QString& presetId)
    {
        QVERIFY(db->setMappingAssignment(group, QStringLiteral("group_default"), QString(),
                                         presetId));
    }

    QString installGamePreset(const QString& group, const QString& gamePath,
                              const QString& name, const QString& actionId,
                              const QString& trigger)
    {
        const QString preset = makePreset(group, name, {presetRow(actionId, trigger)});
        db->setMappingAssignment(group, QStringLiteral("game"),
                                 MappingAssignmentResolver::canonicalGameKey(gamePath), preset);
        return preset;
    }

    bool tableBinds(const QString& group, const QString& profile, const QString& actionId,
                    const QString& trigger)
    {
        for (const BindingResolver::Binding& binding : rt().effectiveBindings(group, profile)) {
            if (binding.actionId == actionId && binding.triggerCode == trigger)
                return true;
        }
        return false;
    }

    // The one wiring the app performs, in the same order: the session decides the
    // key, the binder pushes it, the engine re-resolves at a safe boundary.
    void syncSession() { binder->syncFromSession(); }

    // Storage refuses the two damaged states this suite must still survive: a row
    // whose preset was deleted underneath it, and a row pointing at another
    // group's preset. Both are written out of band, exactly like a database
    // damaged outside the app (same approach as tst_mappingassignmentresolver).
    bool rawExec(const QString& sql)
    {
        bool ok = false;
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                         QStringLiteral("session-raw"));
            raw.setDatabaseName(dbPath);
            if (raw.open()) {
                QSqlQuery query(raw);
                ok = query.exec(sql);
                raw.close();
            }
        }
        QSqlDatabase::removeDatabase(QStringLiteral("session-raw"));
        return ok;
    }

    std::unique_ptr<ConfigManager> config;
    std::unique_ptr<CaptureDatabase> db;
    std::unique_ptr<InputEngine> engine;
    std::unique_ptr<CurrentGameService> session;
    std::unique_ptr<GameSessionPresetBinder> binder;
    std::unique_ptr<MappingPresetModel> model;
    SessionSyntheticXInput* pad = nullptr;
    QString profile;
    QString dbPath;
    std::unique_ptr<QTemporaryDir> dir;

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void sessionKeyFollowsTheRunningGame();
    void overlayOpenKeepsTheGameContext();
    void gameChangeSwitchesAtTheSeam();
    void gameExitRevealsTheLowerWinner();
    void gameExitRevealsTheTypedControllerAssignment();
    void brokenGameAssignmentFallsThroughAndReportsStale();
    void missingGameAssignmentIsVisibleInSettings();
    void sessionRefreshNeverSwitchesOnItsOwn();
    void sameGameLearnsItsExecutableWithoutATransition();
};

void GameSessionPresetTest::initTestCase()
{
    // The running-process fallback compares canonical executable paths; the test
    // binary's own path must therefore canonicalize to something non-empty.
    QVERIFY(!GameIdentity::executableKey(QCoreApplication::applicationFilePath()).isEmpty());
}

void GameSessionPresetTest::init()
{
    dir = std::make_unique<QTemporaryDir>();
    QVERIFY(dir->isValid());
    config = std::make_unique<ConfigManager>(dir->filePath("config.json"));
    dbPath = dir->filePath("test.db");
    db = std::make_unique<CaptureDatabase>(dbPath);
    QVERIFY(db->open());
    QVERIFY(db->clearAllBindingOverrides());
    engine = std::make_unique<InputEngine>(config.get(), db.get(), nullptr);

    auto synthetic = std::make_unique<SessionSyntheticXInput>();
    pad = synthetic.get();
    engine->m_xinputPad = pad;
    engine->attachGamepad(std::move(synthetic), "Synthetic XInput");
    pad->buttons(0);
    pad->setKnownDeviceIdentity(0, "endpoint-session", "2dc8:6001");
    engine->observeLegacyBackend(pad, pad->profile());
    profile = engine->canonicalProfile(pad, "xinput.slot0");
    QVERIFY(!profile.isEmpty());
    engine->activateBackend(pad, QStringLiteral("test setup"));
    rt().reload();
    rt().setDefaultHoldMs(250);
    rt().setTiming({60, 120, 250});
    engine->m_lastControllerRoute = profile;

    session = std::make_unique<CurrentGameService>(db.get());
    binder = std::make_unique<GameSessionPresetBinder>(
        engine.get(), [this] { return session->currentGameExecutableKey(); });
    model = std::make_unique<MappingPresetModel>(db.get());
    model->setGameTargetProvider([this] {
        MappingPresetModel::GameTarget target;
        target.key = session->currentGameExecutableKey();
        target.label = session->currentGameName();
        target.rowId = session->currentGameId();
        return target;
    });
}

void GameSessionPresetTest::cleanup()
{
    model.reset();
    binder.reset();
    session.reset();
    engine.reset();
    db.reset();
    config.reset();
    dir.reset();
}

// The session key is the canonical key of whatever game the session holds - the
// same normalization the assignment rows are written with.
void GameSessionPresetTest::sessionKeyFollowsTheRunningGame()
{
    const QString alpha = offlineGameExecutable(QStringLiteral("Alpha"));
    const QString beta = offlineGameExecutable(QStringLiteral("Beta"));
    QVERIFY(capturedGame(QStringLiteral("Alpha"), alpha) > 0);
    QVERIFY(capturedGame(QStringLiteral("Beta"), beta) > 0);

    QCOMPARE(session->currentGameExecutableKey(), QString());
    QCOMPARE(session->currentGameName(), QString());

    QVERIFY(session->update(QStringLiteral("Alpha"), alpha));
    QCOMPARE(session->currentGameExecutableKey(),
             MappingAssignmentResolver::canonicalGameKey(alpha));
    QCOMPARE(session->currentGameName(), QStringLiteral("Alpha"));

    QVERIFY(session->update(QStringLiteral("Beta"), beta));
    QCOMPARE(session->currentGameExecutableKey(),
             MappingAssignmentResolver::canonicalGameKey(beta));

    // Back to no game in the foreground, and the game is not running either: the
    // session clears after the documented foreground-miss grace.
    QVERIFY(!session->update(QString(), QString()));
    QVERIFY(!session->update(QString(), QString()));
    QVERIFY(session->update(QString(), QString()));
    QCOMPARE(session->currentGameExecutableKey(), QString());
    QCOMPARE(session->currentGameName(), QString());
}

// Opening the overlay must not retarget anything. The overlay is an excluded
// process, so the foreground detector reports "no game" while it is open - and
// the SESSION still holds the game, because the game process is running.
void GameSessionPresetTest::overlayOpenKeepsTheGameContext()
{
    const QString self = QCoreApplication::applicationFilePath();
    QVERIFY(capturedGame(QStringLiteral("SelfGame"), self) > 0);
    const QString gamePreset = installGamePreset(kController, self, QStringLiteral("Game"),
                                                 QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth);
    QVERIFY(!gamePreset.isEmpty());
    const QString groupPreset = makePreset(kController, QStringLiteral("Group"),
                                           {presetRow(QStringLiteral("global.save_replay"),
                                                      ControlId::FaceSouth)});
    QVERIFY(!groupPreset.isEmpty());
    assignGroupDefault(kController, groupPreset);

    QVERIFY(session->update(QStringLiteral("SelfGame"), self));
    syncSession();
    QCOMPARE(engine->runningGameKey(), MappingAssignmentResolver::canonicalGameKey(self));
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("game"));
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gamePreset);

    // The overlay opens: GameHQ is now the foreground window, which is an
    // excluded process, so the detector reports an empty game for as long as the
    // user browses. The game process is still running.
    engine->setOverlayVisible(true);
    for (int tick = 0; tick < 6; ++tick) {
        session->update(QString(), QString());
        syncSession();
        QCOMPARE(engine->runningGameKey(), MappingAssignmentResolver::canonicalGameKey(self));
        QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("game"));
        QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gamePreset);
        QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                           ControlId::FaceSouth));
    }

    // Capture navigation inside the overlay is not a session transition either.
    engine->setOverlayVisible(false);
    syncSession();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gamePreset);
}

// A game change is a session change: the new game's winner is published at the
// cpo-p05 safe boundary, not by replacing the table directly.
void GameSessionPresetTest::gameChangeSwitchesAtTheSeam()
{
    const QString alpha = offlineGameExecutable(QStringLiteral("Alpha"));
    const QString beta = offlineGameExecutable(QStringLiteral("Beta"));
    QVERIFY(capturedGame(QStringLiteral("Alpha"), alpha) > 0);
    QVERIFY(capturedGame(QStringLiteral("Beta"), beta) > 0);
    const QString alphaPreset = installGamePreset(kController, alpha, QStringLiteral("Alpha"),
                                                  QStringLiteral("global.screenshot"),
                                                  ControlId::FaceSouth);
    QVERIFY(!alphaPreset.isEmpty());
    const QString betaPreset = installGamePreset(kController, beta, QStringLiteral("Beta"),
                                                 QStringLiteral("global.save_replay"),
                                                 ControlId::FaceSouth);
    QVERIFY(!betaPreset.isEmpty());

    QVERIFY(session->update(QStringLiteral("Alpha"), alpha));
    syncSession();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), alphaPreset);
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::FaceSouth));

    // A press held while the game changes is gated: it must not act under the new
    // game's table (the switch boundary, not a table swap).
    pad->buttons(1u << Gamepad::Cross);
    QVERIFY(session->update(QStringLiteral("Beta"), beta));
    syncSession();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), betaPreset);
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("game"));
    QVERIFY(engine->mappingArmedReleaseGates().contains(
        kController + QChar(0x1f) + profile + QChar(0x1f) + ControlId::FaceSouth));
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.save_replay"),
                       ControlId::FaceSouth));
    pad->buttons(0);
}

// Game exit removes only the game-context winner; the chain below it serves.
void GameSessionPresetTest::gameExitRevealsTheLowerWinner()
{
    const QString alpha = offlineGameExecutable(QStringLiteral("Alpha"));
    QVERIFY(capturedGame(QStringLiteral("Alpha"), alpha) > 0);
    const QString gamePreset = installGamePreset(kController, alpha, QStringLiteral("Game"),
                                                 QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth);
    QVERIFY(!gamePreset.isEmpty());
    const QString groupPreset = makePreset(kController, QStringLiteral("Group"),
                                           {presetRow(QStringLiteral("global.save_replay"),
                                                      ControlId::FaceSouth)});
    QVERIFY(!groupPreset.isEmpty());
    assignGroupDefault(kController, groupPreset);

    QVERIFY(session->update(QStringLiteral("Alpha"), alpha));
    syncSession();
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("game"));
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gamePreset);

    // The game exits: the foreground leaves it and the process is gone.
    QVERIFY(!session->update(QString(), QString()));
    syncSession();
    QCOMPARE(engine->runningGameKey(), MappingAssignmentResolver::canonicalGameKey(alpha));
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gamePreset);

    QVERIFY(!session->update(QString(), QString()));
    syncSession();
    QVERIFY(session->update(QString(), QString()));
    syncSession();
    QCOMPARE(session->currentGameExecutableKey(), QString());
    QCOMPARE(engine->runningGameKey(), QString());
    QCOMPARE(engine->mappingEffectiveSource(kController, profile),
             QStringLiteral("group_default"));
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), groupPreset);
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.save_replay"),
                       ControlId::FaceSouth));

    // And with no group default the resolver falls all the way to builtin.
    QVERIFY(db->clearMappingAssignment(kController, QStringLiteral("group_default"), QString()));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("builtin"));
    QVERIFY(engine->mappingInstalledPresetId(kController, profile).isEmpty());
}

// The step below the game can be a typed controller assignment, not only the
// group default: exiting the game reveals the durable controller winner.
void GameSessionPresetTest::gameExitRevealsTheTypedControllerAssignment()
{
    const QString alpha = offlineGameExecutable(QStringLiteral("Alpha"));
    QVERIFY(capturedGame(QStringLiteral("Alpha"), alpha) > 0);
    const QString gamePreset = installGamePreset(kController, alpha, QStringLiteral("Game"),
                                                 QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth);
    QVERIFY(!gamePreset.isEmpty());
    const QString slotPreset = makePreset(kController, QStringLiteral("Slot"),
                                          {presetRow(QStringLiteral("global.save_replay"),
                                                     ControlId::FaceSouth)});
    QVERIFY(!slotPreset.isEmpty());
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), slotPreset));

    QVERIFY(session->update(QStringLiteral("Alpha"), alpha));
    syncSession();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gamePreset);

    session->update(QString(), QString());
    session->update(QString(), QString());
    session->update(QString(), QString());
    syncSession();
    QCOMPARE(session->currentGameExecutableKey(), QString());
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("controller"));
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), slotPreset);
}

// A game assignment whose preset is gone (or belongs to another group) is skipped
// once, reported as stale, and the chain below serves - never a broken table and
// never a silent wrong preset.
void GameSessionPresetTest::brokenGameAssignmentFallsThroughAndReportsStale()
{
    const QString alpha = offlineGameExecutable(QStringLiteral("Alpha"));
    QVERIFY(capturedGame(QStringLiteral("Alpha"), alpha) > 0);
    const QString gamePreset = installGamePreset(kController, alpha, QStringLiteral("Game"),
                                                 QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth);
    QVERIFY(!gamePreset.isEmpty());
    const QString groupPreset = makePreset(kController, QStringLiteral("Group"),
                                           {presetRow(QStringLiteral("global.save_replay"),
                                                      ControlId::FaceSouth)});
    QVERIFY(!groupPreset.isEmpty());
    assignGroupDefault(kController, groupPreset);

    QVERIFY(session->update(QStringLiteral("Alpha"), alpha));
    syncSession();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gamePreset);

    // The assigned preset disappears underneath the running game.
    QVERIFY(rawExec(QStringLiteral("DELETE FROM mapping_presets WHERE id = '%1'")
                        .arg(gamePreset)));
    engine->m_assignmentResolver->clearStaleReports();
    engine->refreshResolvedPreset();

    QCOMPARE(engine->mappingEffectiveSource(kController, profile),
             QStringLiteral("group_default"));
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), groupPreset);
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.save_replay"),
                       ControlId::FaceSouth));
    const auto stale = engine->m_assignmentResolver->staleReports();
    QCOMPARE(stale.size(), 1);
    QCOMPARE(stale.first().targetKind, QStringLiteral("game"));
    QCOMPARE(stale.first().reason, QStringLiteral("missing_preset"));

    // Settings says the same thing out loud for the session game.
    model->refreshGameTarget();
    QVERIFY(model->gameAvailable());
    QVERIFY(model->gameAssignedPresetMissing());
    QCOMPARE(model->gameAssignedPresetId(), gamePreset);
}

// The same broken row is visible to the Settings surface, which must offer the
// picker instead of pretending the game follows the chain.
void GameSessionPresetTest::missingGameAssignmentIsVisibleInSettings()
{
    const QString alpha = offlineGameExecutable(QStringLiteral("Alpha"));
    QVERIFY(capturedGame(QStringLiteral("Alpha"), alpha) > 0);
    const QString groupPreset = makePreset(kController, QStringLiteral("Group"),
                                           {presetRow(QStringLiteral("global.save_replay"),
                                                      ControlId::FaceSouth)});
    QVERIFY(!groupPreset.isEmpty());
    assignGroupDefault(kController, groupPreset);

    model->refreshGameTarget();
    QVERIFY(!model->gameAvailable());
    QVERIFY(model->gameAssignedToFallback());

    QVERIFY(session->update(QStringLiteral("Alpha"), alpha));
    model->refreshGameTarget();
    QVERIFY(model->gameAvailable());
    QCOMPARE(model->gameLabel(), QStringLiteral("Alpha"));
    QVERIFY(model->gameAssignedToFallback());

    QVERIFY(model->applyGameAssignment(groupPreset));
    QCOMPARE(model->gameAssignedPresetId(), groupPreset);
    QCOMPARE(model->gameAssignedPresetName(), QStringLiteral("Group"));

    // Wrong-group rows are surfaced, not silently followed. Storage refuses this
    // pairing, so a database damaged outside the app writes it out of band - the
    // Settings surface must still name it instead of showing a silent fallback.
    const QString keyboardOnly = makePreset(kKeyboard, QStringLiteral("Keys"), {});
    QVERIFY(!keyboardOnly.isEmpty());
    QVERIFY(rawExec(QStringLiteral(
        "INSERT OR REPLACE INTO mapping_assignments "
        "(device_group, target_kind, target_key, preset_id, game_row_id, created_at, updated_at) "
        "VALUES ('controller', 'game', '%1', '%2', NULL, '2026-09-20', '2026-09-20')")
                        .arg(MappingAssignmentResolver::canonicalGameKey(alpha), keyboardOnly)));
    model->refreshGameTarget();
    QVERIFY(model->gameAssignedPresetWrongGroup());
    QCOMPARE(model->gameAssignedPresetId(), keyboardOnly);
}

// Re-publishing the session state is not a switch: it never touches the runtime
// seam, so a foreground poll that resolves to the same game cannot invalidate a
// gesture.
void GameSessionPresetTest::sessionRefreshNeverSwitchesOnItsOwn()
{
    const QString alpha = offlineGameExecutable(QStringLiteral("Alpha"));
    QVERIFY(capturedGame(QStringLiteral("Alpha"), alpha) > 0);
    installGamePreset(kController, alpha, QStringLiteral("Game"),
                      QStringLiteral("global.screenshot"), ControlId::FaceSouth);

    int refreshes = 0;
    model->setRuntimeRefresh([&refreshes] { ++refreshes; });
    QVERIFY(session->update(QStringLiteral("Alpha"), alpha));
    model->refreshGameTarget();
    syncSession();
    QCOMPARE(refreshes, 0);

    // A real game assignment does go through the seam.
    const QString groupPreset = makePreset(kController, QStringLiteral("Group"),
                                           {presetRow(QStringLiteral("global.save_replay"),
                                                      ControlId::FaceSouth)});
    QVERIFY(!groupPreset.isEmpty());
    QVERIFY(model->applyGameAssignment(groupPreset));
    QCOMPARE(refreshes, 1);
    QCOMPARE(engine->runningGameKey(), MappingAssignmentResolver::canonicalGameKey(alpha));
}

// The SAME session game can learn - or change - its stored executable identity
// with NO game transition. CurrentGameService::update() reports no state change
// for that (the game id and its capture state did not move), so the app announces
// it through gamesChanged() alone. The cpo-p07 review found exactly this hole:
// with only currentGameChanged wired, the engine and the Settings game row kept
// the old (or empty) key until some unrelated later transition, so a per-game
// assignment stayed inert in the meantime. This drives the SHARED wiring the app
// uses, so removing either pair of connections fails here.
void GameSessionPresetTest::sameGameLearnsItsExecutableWithoutATransition()
{
    const QString alpha = offlineGameExecutable(QStringLiteral("Alpha"));

    // The game is capturable (its row comes from a capture) but has no remembered
    // executable yet, so no game key can be built from the session.
    QVERIFY(capturedGame(QStringLiteral("Alpha"), QString()) > 0);

    // The app's wiring, the same function App.cpp calls.
    SessionSignalsStub sessionSignals;
    GameSessionPresetBinder::wireGameSessionContext(&sessionSignals, binder.get(), model.get());

    // A per-game assignment for the identity the row is about to learn - written
    // in an earlier session, when the path was known - plus a group default for
    // every phase where the game rule cannot win (no key at all, and later a key
    // nothing is assigned to).
    const QString gamePreset = installGamePreset(kController, alpha, QStringLiteral("Game"),
                                                 QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth);
    QVERIFY(!gamePreset.isEmpty());
    const QString groupPreset = makePreset(kController, QStringLiteral("Group"),
                                           {presetRow(QStringLiteral("global.save_replay"),
                                                      ControlId::FaceEast)});
    QVERIFY(!groupPreset.isEmpty());
    assignGroupDefault(kController, groupPreset);

    // Session enters the game: a real transition, but the row still carries no
    // executable, so there is no game key - and no game context in the engine.
    const bool entered = session->update(QStringLiteral("Alpha"), QString());
    QVERIFY(entered);
    sessionSignals.publish(entered, session->lastUpdateChangedGameMetadata());
    QCOMPARE(engine->runningGameKey(), QString());
    QVERIFY(!model->gameAvailable());

    // The row learns its executable. Same game id, same capture state: NOT a
    // transition - update() says so, and only the metadata flag moves.
    const bool transitioned = session->update(QStringLiteral("Alpha"), alpha);
    QVERIFY(!transitioned);
    QVERIFY(session->lastUpdateChangedGameMetadata());

    // So the app emits gamesChanged() and nothing else, and that alone must move
    // the engine onto the game assignment and the Settings row onto the game.
    sessionSignals.publish(transitioned, session->lastUpdateChangedGameMetadata());
    QCOMPARE(engine->runningGameKey(), MappingAssignmentResolver::canonicalGameKey(alpha));
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("game"));
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gamePreset);
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::FaceSouth));
    QVERIFY(!tableBinds(kController, profile, QStringLiteral("global.save_replay"),
                        ControlId::FaceEast));
    QVERIFY(model->gameAvailable());
    QCOMPARE(model->gameLabel(), QStringLiteral("Alpha"));
    QCOMPARE(model->gameAssignedPresetName(), QStringLiteral("Game"));

    // The same game row can also CHANGE its executable (reinstalled, moved):
    // still no transition, still gamesChanged() alone - the key follows the row,
    // and the old key's assignment must stop serving.
    const QString moved = offlineGameExecutable(QStringLiteral("Alpha-Moved"));
    const bool relocated = session->update(QStringLiteral("Alpha"), moved);
    QVERIFY(!relocated);
    QVERIFY(session->lastUpdateChangedGameMetadata());
    sessionSignals.publish(relocated, session->lastUpdateChangedGameMetadata());
    QCOMPARE(engine->runningGameKey(), MappingAssignmentResolver::canonicalGameKey(moved));
    QCOMPARE(engine->mappingEffectiveSource(kController, profile),
             QStringLiteral("group_default"));
    QVERIFY(!tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                        ControlId::FaceSouth));
    QCOMPARE(model->gameLabel(), QStringLiteral("Alpha"));
    QVERIFY(model->gameAssignedToFallback());

    // Re-publishing a library change is not a switch: the key is unchanged, so
    // the runtime seam is never touched again (rescan, capture commits and
    // deletions all emit gamesChanged).
    int refreshes = 0;
    model->setRuntimeRefresh([&refreshes] { ++refreshes; });
    sessionSignals.publish(false, true);
    QCOMPARE(engine->runningGameKey(), MappingAssignmentResolver::canonicalGameKey(moved));
    QCOMPARE(refreshes, 0);
}

QTEST_MAIN(GameSessionPresetTest)
#include "tst_gamesessionpresets.moc"
