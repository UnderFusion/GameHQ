#include "overlay/OverlayLifetimePolicy.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QtTest>

#ifndef GAMEHQ_SOURCE_DIR
#error "tst_overlaylifetime needs GAMEHQ_SOURCE_DIR (see tests/CMakeLists.txt)"
#endif

// cpo-o03: the overlay's lifetime/focus rules, with every Win32 query replaced
// by the fact it would resolve to. src/overlay/OverlayManager.cpp is the only
// place those facts come from real Windows; the point here is that the rules
// themselves — which foreground changes keep the overlay open, which rebind it
// to a replaced game window, and which dismiss it — are decided in exactly one
// place, and that the response stays on the existing presenter.
//
// The popup case is a structural audit: the per-capture action menu and the
// delete confirmation are in-window QML items today. If either ever becomes a
// real top-level window it must leave this file with an explicit non-activation
// assertion next to it (see OverlayPresenter), not slide in silently.
namespace
{
void* hwnd(quintptr id) { return reinterpret_cast<void*>(id); }

// A foreground window that behaves like a running game's main window: valid,
// visible, unminimized, unowned and top-level.
OverlayLifetime::ForegroundFacts gameLikeWindow()
{
    OverlayLifetime::ForegroundFacts facts;
    facts.validWindow = true;
    facts.visible = true;
    facts.topLevel = true;
    return facts;
}

// The window we currently remember as the game's, in good health: alive,
// visible, unminimized, still top-level and unowned — the state in which it
// must keep its place as the game's window.
void markRememberedGameHealthy(OverlayLifetime::ForegroundFacts& facts)
{
    facts.rememberedGameAlive = true;
    facts.rememberedGameVisible = true;
    facts.rememberedGameIconic = false;
    facts.rememberedGameTopLevel = true;
    facts.rememberedGameOwned = false;
}

class RecordingActions final : public OverlayLifetime::Actions
{
public:
    QStringList calls;
    QList<void*> reboundWindows;

    void rebindGameWindow(void* newWindow) override
    {
        calls << QStringLiteral("rebind");
        reboundWindows << newWindow;
    }

    void reassertOverlay() override { calls << QStringLiteral("reassert"); }
    void repositionOverlay() override { calls << QStringLiteral("reposition"); }
    void hideOverlay() override { calls << QStringLiteral("hide"); }
};

QString readSource(const QString& relativePath)
{
    QFile file(QDir(QString::fromUtf8(GAMEHQ_SOURCE_DIR)).filePath(relativePath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

// Every declaration that would make a file own a second native window of its
// own: a popup written as one of these takes focus on its own terms again.
QStringList topLevelWindowDeclarations(const QString& source)
{
    static const QRegularExpression declaration(
        QStringLiteral("^\\s*(Window|ApplicationWindow|Popup|Dialog|Menu|ToolTip)\\s*\\{"));

    QStringList found;
    const QStringList lines = source.split(QLatin1Char('\n'));
    for (int index = 0; index < lines.size(); ++index) {
        const QString line = lines.at(index);
        if (line.trimmed().startsWith(QLatin1String("//")))
            continue;
        if (declaration.match(line).hasMatch())
            found << QStringLiteral("%1:%2").arg(index + 1).arg(line.trimmed());
    }
    return found;
}

// The first line that is neither blank, a comment, nor an import: the root
// element of a QML component.
QString rootElement(const QString& source)
{
    const QStringList lines = source.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("//"))
            || trimmed.startsWith(QLatin1String("import "))) {
            continue;
        }
        return trimmed;
    }
    return QString();
}
}  // namespace

class OverlayLifetimeTest : public QObject
{
    Q_OBJECT

private slots:
    void decisionNamesAreStable();

    // --- which foreground changes keep the overlay open ----------------------
    void gameStayingForegroundKeepsOverlayOpen();
    void sameProcessReplacementWindowRebinds();
    void secondTopLevelWindowOfTheGameKeepsTheRememberedOne();
    void destroyedOldHandleStillRecoversThroughProcessIdentity();
    void auxiliarySameProcessPopupKeepsContextWithoutRebinding();
    void overlayOwnForegroundIsIgnored();

    // --- which foreground changes dismiss it ---------------------------------
    void sameProcessMinimizedContextHides();
    void sameProcessHiddenContextHides();
    void minimizedRememberedWindowIsNotReplacedBySameProcessWindow();
    void foreignProcessForegroundHides();
    void destroyedForegroundHides();

    // --- how a decision is applied -------------------------------------------
    void keepAndIgnoreDoNothing();
    void rebindReassertsThroughThePresenter();
    void rebindOnAnotherMonitorRepositionsThroughThePresenter();
    void hideDecisionHides();

    // --- geometry never trusts a stale handle --------------------------------
    void monitorPickMatchesTheGameMonitor();
    void monitorPickFallsBackWithoutAMatch();

    // --- a queued foreground event is only acted on while it is current ------
    void noForegroundWindowReachesThePolicy();
    void staleForegroundEventIsIgnored();
    void staleEventAfterTheGameReturnsIsIgnored();

    // --- the polled game-context check (cpo-o06b) ----------------------------
    void aHealthyRememberedGameIsNotALostContext();
    void aGoneRememberedGameIsALostContext();
    void theShapeOfTheRememberedWindowDoesNotDecideLoss();

    // --- popups stay inside the overlay window -------------------------------
    void overlayPopupsRemainInWindow();
};

void OverlayLifetimeTest::decisionNamesAreStable()
{
    // The manager logs these names; they are the human-readable half of the
    // contract, so they must not drift silently.
    QCOMPARE(QString::fromLatin1(OverlayLifetime::decisionName(OverlayLifetime::Decision::Ignore)),
             QStringLiteral("ignore"));
    QCOMPARE(QString::fromLatin1(OverlayLifetime::decisionName(OverlayLifetime::Decision::Keep)),
             QStringLiteral("keep"));
    QCOMPARE(QString::fromLatin1(OverlayLifetime::decisionName(OverlayLifetime::Decision::RebindGame)),
             QStringLiteral("rebind_game"));
    QCOMPARE(QString::fromLatin1(OverlayLifetime::decisionName(OverlayLifetime::Decision::Hide)),
             QStringLiteral("hide"));
}

void OverlayLifetimeTest::gameStayingForegroundKeepsOverlayOpen()
{
    // The non-activating overlay leaves the game focused: this is the common
    // case and it must never be treated as a focus loss.
    OverlayLifetime::ForegroundFacts facts = gameLikeWindow();
    facts.isRememberedGame = true;
    facts.sameProcessAsGame = true;

    QVERIFY(OverlayLifetime::decide(facts) == OverlayLifetime::Decision::Keep);
}

void OverlayLifetimeTest::sameProcessReplacementWindowRebinds()
{
    // The remembered handle is gone (destroyed), and the game's process put
    // this visible, unowned, top-level window up: THAT is the game's new main
    // window. Keep the overlay open and rebind to it.
    OverlayLifetime::ForegroundFacts facts = gameLikeWindow();
    facts.sameProcessAsGame = true;
    facts.rememberedGameAlive = false;

    QVERIFY(OverlayLifetime::decide(facts) == OverlayLifetime::Decision::RebindGame);
}

void OverlayLifetimeTest::secondTopLevelWindowOfTheGameKeepsTheRememberedOne()
{
    // The remembered main window is still healthy. When the same process puts
    // ANOTHER unowned top-level window in the foreground (launcher surface,
    // splash, tool window, error surface), that window is not a replacement:
    // the overlay stays open and keeps measuring the remembered handle.
    OverlayLifetime::ForegroundFacts second = gameLikeWindow();
    second.sameProcessAsGame = true;
    markRememberedGameHealthy(second);

    QVERIFY(OverlayLifetime::decide(second) == OverlayLifetime::Decision::Keep);

    // Keep is the whole effect: nothing is rebound, re-presented or hidden.
    RecordingActions actions;
    OverlayLifetime::apply(OverlayLifetime::Decision::Keep, OverlayLifetime::RebindRequest{}, actions);
    QVERIFY2(actions.calls.isEmpty(), qPrintable(actions.calls.join(QLatin1Char(','))));
}

void OverlayLifetimeTest::destroyedOldHandleStillRecoversThroughProcessIdentity()
{
    // A window that no longer exists is not a context: it is never remembered
    // (and never used for geometry or as a desktop-handoff return address).
    OverlayLifetime::ForegroundFacts destroyed;
    destroyed.validWindow = false;
    QVERIFY(OverlayLifetime::decide(destroyed) == OverlayLifetime::Decision::Hide);

    // Recovery does not need the old handle to be alive — the manager keeps the
    // process id across the old handle's death, and the replacement window is
    // recognised by that identity alone.
    OverlayLifetime::ForegroundFacts replacement = gameLikeWindow();
    replacement.sameProcessAsGame = true;
    QVERIFY(OverlayLifetime::decide(replacement) == OverlayLifetime::Decision::RebindGame);
}

void OverlayLifetimeTest::auxiliarySameProcessPopupKeepsContextWithoutRebinding()
{
    // An owned popup of the game (a dialog it opened) is still inside the game
    // context — the overlay stays — but it must NOT become the remembered game
    // window, or the overlay would later measure and follow the wrong handle.
    OverlayLifetime::ForegroundFacts popup = gameLikeWindow();
    popup.sameProcessAsGame = true;
    popup.owned = true;
    QVERIFY(OverlayLifetime::decide(popup) == OverlayLifetime::Decision::Keep);

    // Same for a child window of the game.
    OverlayLifetime::ForegroundFacts child = gameLikeWindow();
    child.sameProcessAsGame = true;
    child.topLevel = false;
    QVERIFY(OverlayLifetime::decide(child) == OverlayLifetime::Decision::Keep);
}

void OverlayLifetimeTest::overlayOwnForegroundIsIgnored()
{
    // The overlay grabbing its own foreground during show() is expected.
    OverlayLifetime::ForegroundFacts facts = gameLikeWindow();
    facts.isOverlay = true;
    QVERIFY(OverlayLifetime::decide(facts) == OverlayLifetime::Decision::Ignore);
}

void OverlayLifetimeTest::sameProcessMinimizedContextHides()
{
    // Process identity is evidence of continuity, not permission to ignore a
    // genuine loss of game context: a minimized same-process window hides the
    // overlay, both when it is the remembered window and when it is a
    // replacement that came up minimized.
    OverlayLifetime::ForegroundFacts remembered = gameLikeWindow();
    remembered.isRememberedGame = true;
    remembered.sameProcessAsGame = true;
    remembered.iconic = true;
    QVERIFY(OverlayLifetime::decide(remembered) == OverlayLifetime::Decision::Hide);

    OverlayLifetime::ForegroundFacts replacement = gameLikeWindow();
    replacement.sameProcessAsGame = true;
    replacement.iconic = true;
    QVERIFY(OverlayLifetime::decide(replacement) == OverlayLifetime::Decision::Hide);
}

void OverlayLifetimeTest::sameProcessHiddenContextHides()
{
    OverlayLifetime::ForegroundFacts facts = gameLikeWindow();
    facts.sameProcessAsGame = true;
    facts.visible = false;
    QVERIFY(OverlayLifetime::decide(facts) == OverlayLifetime::Decision::Hide);
}

void OverlayLifetimeTest::minimizedRememberedWindowIsNotReplacedBySameProcessWindow()
{
    // The remembered window still exists but stopped showing the game. A
    // same-process top-level window in the foreground is a bystander then, not
    // a proven replacement: it must never silently become the new main context
    // (that would keep the overlay open over a minimized game).
    OverlayLifetime::ForegroundFacts minimized = gameLikeWindow();
    minimized.sameProcessAsGame = true;
    markRememberedGameHealthy(minimized);
    minimized.rememberedGameIconic = true;  // the game minimized itself
    QVERIFY(OverlayLifetime::decide(minimized) == OverlayLifetime::Decision::Hide);

    // Same when the game hid its window instead of minimizing it.
    OverlayLifetime::ForegroundFacts hidden = gameLikeWindow();
    hidden.sameProcessAsGame = true;
    markRememberedGameHealthy(hidden);
    hidden.rememberedGameVisible = false;
    QVERIFY(OverlayLifetime::decide(hidden) == OverlayLifetime::Decision::Hide);

    // Only a GONE remembered handle makes a same-process window the game's
    // replacement (window recreation).
    OverlayLifetime::ForegroundFacts recreated = gameLikeWindow();
    recreated.sameProcessAsGame = true;  // remembered handle destroyed (defaults)
    QVERIFY(OverlayLifetime::decide(recreated) == OverlayLifetime::Decision::RebindGame);
}

void OverlayLifetimeTest::foreignProcessForegroundHides()
{
    // The desktop, the shell, the task switcher and any other application all
    // arrive as "a valid window of another process" — and all of them dismiss
    // the overlay.
    OverlayLifetime::ForegroundFacts shell = gameLikeWindow();
    QVERIFY(OverlayLifetime::decide(shell) == OverlayLifetime::Decision::Hide);

    // A foreign window that is itself minimized or owned is still foreign.
    OverlayLifetime::ForegroundFacts foreignPopup = gameLikeWindow();
    foreignPopup.owned = true;
    QVERIFY(OverlayLifetime::decide(foreignPopup) == OverlayLifetime::Decision::Hide);

    // And so is no foreground at all.
    OverlayLifetime::ForegroundFacts none;
    QVERIFY(OverlayLifetime::decide(none) == OverlayLifetime::Decision::Hide);
}

void OverlayLifetimeTest::destroyedForegroundHides()
{
    OverlayLifetime::ForegroundFacts facts;
    facts.validWindow = false;
    facts.sameProcessAsGame = true;  // even a same-process handle that is gone
    QVERIFY(OverlayLifetime::decide(facts) == OverlayLifetime::Decision::Hide);
}

void OverlayLifetimeTest::keepAndIgnoreDoNothing()
{
    for (const OverlayLifetime::Decision decision : { OverlayLifetime::Decision::Ignore,
                                                      OverlayLifetime::Decision::Keep }) {
        RecordingActions actions;
        OverlayLifetime::apply(decision, OverlayLifetime::RebindRequest{}, actions);
        QVERIFY2(actions.calls.isEmpty(), qPrintable(actions.calls.join(QLatin1Char(','))));
    }
}

void OverlayLifetimeTest::rebindReassertsThroughThePresenter()
{
    RecordingActions actions;
    OverlayLifetime::RebindRequest rebind;
    rebind.newWindow = hwnd(0x201);
    rebind.otherMonitor = false;

    OverlayLifetime::apply(OverlayLifetime::Decision::RebindGame, rebind, actions);

    // Rebind first (the game is now that window), then re-assert the
    // never-activate guarantee on the presentation that is already up.
    QCOMPARE(actions.calls, QStringList({ QStringLiteral("rebind"),
                                          QStringLiteral("reassert") }));
    QCOMPARE(actions.reboundWindows, QList<void*>({ hwnd(0x201) }));
}

void OverlayLifetimeTest::rebindOnAnotherMonitorRepositionsThroughThePresenter()
{
    RecordingActions actions;
    OverlayLifetime::RebindRequest rebind;
    rebind.newWindow = hwnd(0x202);
    rebind.otherMonitor = true;

    OverlayLifetime::apply(OverlayLifetime::Decision::RebindGame, rebind, actions);

    // The overlay follows the game, but through a presentation (presenter owns
    // geometry and the never-activate guarantee) — never a bare move.
    QCOMPARE(actions.calls, QStringList({ QStringLiteral("rebind"),
                                          QStringLiteral("reposition") }));
}

void OverlayLifetimeTest::hideDecisionHides()
{
    RecordingActions actions;
    OverlayLifetime::apply(OverlayLifetime::Decision::Hide, OverlayLifetime::RebindRequest{},
                           actions);
    QCOMPARE(actions.calls, QStringList({ QStringLiteral("hide") }));
}

void OverlayLifetimeTest::noForegroundWindowReachesThePolicy()
{
    // Windows can genuinely have no foreground window for a moment — a game
    // minimizing or being destroyed with nothing else focused yet. The event
    // that carries that state must be admitted (nullptr == nullptr), and the
    // policy must hide: the overlay cannot stay up without a game context.
    QVERIFY(OverlayLifetime::isCurrentForegroundEvent(nullptr, nullptr));

    OverlayLifetime::ForegroundFacts facts;  // no foreground resolved at all
    QCOMPARE(OverlayLifetime::decide(facts), OverlayLifetime::Decision::Hide);

    // The manager's half of the contract: it routes the event through the
    // admission helper instead of dropping a null foreground early — that
    // early return is exactly what made the Hide rule unreachable.
    const QString manager = readSource(QStringLiteral("overlay/OverlayManager.cpp"));
    QVERIFY2(!manager.isEmpty(), "OverlayManager.cpp must be readable through GAMEHQ_SOURCE_DIR");
    QVERIFY(manager.contains(
        QLatin1String("OverlayLifetime::isCurrentForegroundEvent(newForeground")));
    QVERIFY2(!manager.contains(QLatin1String("!foreground ||")),
             "the null-foreground early return must not come back");
}

void OverlayLifetimeTest::staleForegroundEventIsIgnored()
{
    // A queued event whose window is no longer the foreground is stale: it
    // must not be resolved as the current context, whether the foreground has
    // moved to a third window or vanished entirely.
    QVERIFY(!OverlayLifetime::isCurrentForegroundEvent(hwnd(0x201), hwnd(0x100)));
    QVERIFY(!OverlayLifetime::isCurrentForegroundEvent(hwnd(0x201), nullptr));

    // The matching event is still admitted, so this is about staleness, not
    // about ignoring foreground events.
    QVERIFY(OverlayLifetime::isCurrentForegroundEvent(hwnd(0x201), hwnd(0x201)));
}

void OverlayLifetimeTest::staleEventAfterTheGameReturnsIsIgnored()
{
    // The event from the moment the game lost focus is still queued after the
    // game has already taken the foreground back: it is stale, and acting on
    // it would resolve a context that no longer exists.
    void* const rememberedGame = hwnd(0x100);
    QVERIFY(!OverlayLifetime::isCurrentForegroundEvent(hwnd(0x201), rememberedGame));

    // The current state is the remembered game foreground, which keeps the
    // overlay open — unchanged by the stale event.
    OverlayLifetime::ForegroundFacts facts = gameLikeWindow();
    facts.isRememberedGame = true;
    markRememberedGameHealthy(facts);
    QCOMPARE(OverlayLifetime::decide(facts), OverlayLifetime::Decision::Keep);
}

void OverlayLifetimeTest::monitorPickMatchesTheGameMonitor()
{
    void* monitors[] = { hwnd(0x11), hwnd(0x22), hwnd(0x33) };
    QCOMPARE(OverlayLifetime::screenIndexForMonitor(hwnd(0x22), monitors, 3), 1);
    QCOMPARE(OverlayLifetime::screenIndexForMonitor(hwnd(0x11), monitors, 3), 0);
    QCOMPARE(OverlayLifetime::screenIndexForMonitor(hwnd(0x33), monitors, 3), 2);
}

void OverlayLifetimeTest::monitorPickFallsBackWithoutAMatch()
{
    // -1 means "no monitor matched": the caller covers the primary screen. An
    // unknown or stale game handle can therefore never move the overlay to an
    // invented monitor, and a null monitor id never matches one.
    void* monitors[] = { hwnd(0x11), hwnd(0x22) };
    QCOMPARE(OverlayLifetime::screenIndexForMonitor(hwnd(0x99), monitors, 2), -1);
    QCOMPARE(OverlayLifetime::screenIndexForMonitor(nullptr, monitors, 2), -1);
    QCOMPARE(OverlayLifetime::screenIndexForMonitor(hwnd(0x11), monitors, 0), -1);
    QCOMPARE(OverlayLifetime::screenIndexForMonitor(hwnd(0x11), nullptr, 0), -1);
}

// cpo-o06b: while the overlay owns the foreground, a game losing its window
// produces no foreground event at all — so the manager polls instead. These
// three cases pin the rule that poll uses.
void OverlayLifetimeTest::aHealthyRememberedGameIsNotALostContext()
{
    OverlayLifetime::ForegroundFacts facts;
    facts.rememberedGameAlive = true;
    facts.rememberedGameVisible = true;
    facts.rememberedGameIconic = false;

    QVERIFY(!OverlayLifetime::rememberedGameContextLost(facts));
}

void OverlayLifetimeTest::aGoneRememberedGameIsALostContext()
{
    // Destroyed.
    OverlayLifetime::ForegroundFacts destroyed;
    destroyed.rememberedGameAlive = false;
    QVERIFY(OverlayLifetime::rememberedGameContextLost(destroyed));

    // Alive but no longer showing anything.
    OverlayLifetime::ForegroundFacts hidden;
    hidden.rememberedGameAlive = true;
    hidden.rememberedGameVisible = false;
    QVERIFY(OverlayLifetime::rememberedGameContextLost(hidden));

    // Alive, visible, minimized: the same loss the event path hides on.
    OverlayLifetime::ForegroundFacts minimized;
    minimized.rememberedGameAlive = true;
    minimized.rememberedGameVisible = true;
    minimized.rememberedGameIconic = true;
    QVERIFY(OverlayLifetime::rememberedGameContextLost(minimized));
}

void OverlayLifetimeTest::theShapeOfTheRememberedWindowDoesNotDecideLoss()
{
    // A game briefly reparenting or re-owning its own window is not a game
    // that went away: only destroyed / hidden / minimized counts. Reading
    // shape here would close the overlay during ordinary window churn.
    OverlayLifetime::ForegroundFacts facts;
    facts.rememberedGameAlive = true;
    facts.rememberedGameVisible = true;
    facts.rememberedGameIconic = false;
    facts.rememberedGameTopLevel = false;
    facts.rememberedGameOwned = true;

    QVERIFY(!OverlayLifetime::rememberedGameContextLost(facts));
}

void OverlayLifetimeTest::overlayPopupsRemainInWindow()
{
    const QString shell = readSource(QStringLiteral("ui/qml/OverlayWindow.qml"));
    QVERIFY2(!shell.isEmpty(), "OverlayWindow.qml must be readable through GAMEHQ_SOURCE_DIR");

    // The overlay shell is the ONE top-level window of the overlay; anything
    // else declared as a Window/Popup/Dialog here would be a second native
    // window with its own activation behaviour.
    const QStringList shellDeclarations = topLevelWindowDeclarations(shell);
    QCOMPARE(shellDeclarations.size(), 1);
    QVERIFY(shellDeclarations.first().contains(QLatin1String("Window {")));

    // Both popups are in-window items, so their lifetime is the overlay's own.
    for (const QString& relative : { QStringLiteral("ui/qml/components/ConfirmDialog.qml"),
                                     QStringLiteral("ui/qml/components/OverlayActionMenu.qml") }) {
        const QString source = readSource(relative);
        QVERIFY2(!source.isEmpty(), qPrintable(relative));
        QVERIFY2(topLevelWindowDeclarations(source).isEmpty(), qPrintable(relative));
        QVERIFY2(rootElement(source).startsWith(QLatin1String("Item")), qPrintable(relative));
    }

    // ... and the overlay actually instantiates them, so the in-window audit is
    // about the real delete-confirm and action-menu paths.
    QVERIFY(shell.contains(QLatin1String("ConfirmDialog {")));
    QVERIFY(shell.contains(QLatin1String("OverlayActionMenu {")));
}

QTEST_MAIN(OverlayLifetimeTest)
#include "tst_overlaylifetime.moc"
