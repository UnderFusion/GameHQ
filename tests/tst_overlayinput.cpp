#include "input/OverlayInputPolicy.h"

#include "input/ActionCatalog.h"
#include "input/ContextOverrideCatalog.h"
#include "input/ControlId.h"
#include "input/DefaultBindings.h"

#include <QtTest>

#include <QFile>
#include <QSet>
#include <QStringList>

// cpo-o04: the GameHQ-side input contract around the overlay.
//
// GameHQ reads the pad passively and never suppresses anything, so these rules
// decide only what *GameHQ* fires for a press — never what the game still
// receives. The audit runs the shipped default table through the same
// arbitration the runtime uses (OverlayInput::select, which
// BindingResolver::matching delegates to), so PS/View/Back/Circle/navigation
// delivery, the escape routes and the one-press-one-action rule are pinned by
// evidence instead of prose.
namespace {

using OverlayInput::Candidate;
using OverlayInput::Context;

// The rows the runtime would consider for a pressed trigger: controller rows
// only, in table order, with their action resolved.
QVector<Candidate> candidatesFor(const QString& trigger)
{
    QVector<Candidate> candidates;
    for (const auto& binding : gamehqDefaultBindings()) {
        if (binding.deviceGroup != QLatin1String("controller")
            || binding.triggerCode != trigger)
            continue;
        const auto* action = ActionCatalog::find(binding.actionId);
        if (!action)
            continue;
        candidates.append({binding.actionId, action->scope, binding.gesture()});
    }
    return candidates;
}

QStringList delivered(const QString& trigger, const GestureSpec& gesture,
                      const Context& context)
{
    return OverlayInput::deliveredActions(candidatesFor(trigger), gesture, context);
}

Context overlayOpen()
{
    return Context{true, false, false};
}

// The body of the QML function declared by `header`, up to the closing brace at
// the declaration's own indentation; empty when the declaration is absent.
QString functionBody(const QString& source, const QString& header)
{
    const qsizetype start = source.indexOf(header);
    if (start < 0)
        return QString();

    const qsizetype lineStart = source.lastIndexOf(QLatin1Char('\n'), start) + 1;
    qsizetype indent = 0;
    while (lineStart + indent < source.size()
           && source.at(lineStart + indent) == QLatin1Char(' '))
        ++indent;

    const QString closer = QStringLiteral("\n") + QString(indent, QLatin1Char(' '))
                           + QLatin1Char('}');
    const qsizetype end = source.indexOf(closer, start);
    if (end < 0)
        return QString();
    return source.mid(start, end + closer.size() - start);
}

} // namespace

class OverlayInputTest : public QObject
{
    Q_OBJECT

private slots:
    void scopeSelection();
    void overlayDeliveryPerControl();
    void desktopFocusCannotRetargetOverlay();
    void escapeRoutes();
    void noDoubleDeliveryInShippedDefaults();
    void overlayShellKeepsSeekAndCaptureIndependent();
};

// The one place scope selection happens; InputEngine::primaryScope/fallbackScope
// return exactly these values.
void OverlayInputTest::scopeSelection()
{
    using Scope = ActionCatalog::Scope;

    const Context idle;
    QCOMPARE(OverlayInput::primaryScope(idle), Scope::Global);
    QCOMPARE(OverlayInput::fallbackScope(idle), Scope::Global);

    const Context desktop{false, false, true};
    QCOMPARE(OverlayInput::primaryScope(desktop), Scope::Desktop);
    QCOMPARE(OverlayInput::fallbackScope(desktop), Scope::Global);

    const Context overlay{true, false, false};
    QCOMPARE(OverlayInput::primaryScope(overlay), Scope::Overlay);
    QCOMPARE(OverlayInput::fallbackScope(overlay), Scope::Global);

    // The overlay wins over desktop focus: GameHQ's own window holding
    // foreground never turns the controller into desktop controls underneath
    // a visible overlay.
    const Context overlayPlusDesktop{true, false, true};
    QCOMPARE(OverlayInput::primaryScope(overlayPlusDesktop), Scope::Overlay);
    QCOMPARE(OverlayInput::fallbackScope(overlayPlusDesktop), Scope::Global);

    const Context playbackOnDesktop{false, true, true};
    QCOMPARE(OverlayInput::primaryScope(playbackOnDesktop), Scope::Playback);
    QCOMPARE(OverlayInput::fallbackScope(playbackOnDesktop), Scope::Desktop);

    const Context playbackInOverlay{true, true, false};
    QCOMPARE(OverlayInput::primaryScope(playbackInOverlay), Scope::Playback);
    QCOMPARE(OverlayInput::fallbackScope(playbackInOverlay), Scope::Overlay);
}

// What GameHQ fires for the named controls while the overlay is visible, from
// the shipped table. Every list is exact: the audit fails if a control ever
// grows a second delivery or loses its documented one.
void OverlayInputTest::overlayDeliveryPerControl()
{
    using namespace ControlId;
    const Context open = overlayOpen();
    const GestureSpec tap = GestureSpec::tap(1);
    // The overlay/desktop navigation defaults are Press activations: they fire
    // on the button-down edge, before any tap/hold timing is known.
    const GestureSpec press = GestureSpec::press();

    // Escape route 1: PS tap toggles the overlay from anywhere.
    QCOMPARE(delivered(Guide, tap, open), QStringList{QStringLiteral("global.toggle_overlay")});
    // PS hold belongs to the desktop handoff, not the overlay.
    QCOMPARE(delivered(Guide, GestureSpec::hold(0), open),
             QStringList{QStringLiteral("global.toggle_desktop")});

    // Share keeps its capture meanings inside the overlay.
    QCOMPARE(delivered(Capture, tap, open), QStringList{QStringLiteral("global.screenshot")});
    QCOMPARE(delivered(Capture, GestureSpec::hold(0), open),
             QStringList{QStringLiteral("global.save_replay")});
    // Escape route 2: Share double-tap.
    QCOMPARE(delivered(Capture, GestureSpec::tap(2), open),
             QStringList{QStringLiteral("global.toggle_overlay")});

    // Escape route 3: Circle backs out (menu first, then playback, then close).
    QCOMPARE(delivered(FaceEast, press, open), QStringList{QStringLiteral("overlay.back")});

    QCOMPARE(delivered(FaceSouth, press, open), QStringList{QStringLiteral("overlay.confirm")});
    // Cross's desktop hold (bulk select) is desktop scope: inert in the overlay.
    QCOMPARE(delivered(FaceSouth, GestureSpec::hold(0), open), QStringList{});
    QCOMPARE(delivered(FaceNorth, press, open), QStringList{QStringLiteral("overlay.favorite")});
    QCOMPARE(delivered(FaceWest, press, open), QStringList{QStringLiteral("overlay.menu")});
    QCOMPARE(delivered(Menu, press, open), QStringList{QStringLiteral("overlay.sidebar_toggle")});

    QCOMPARE(delivered(DpadUp, press, open), QStringList{QStringLiteral("overlay.navigate_up")});
    QCOMPARE(delivered(DpadDown, press, open), QStringList{QStringLiteral("overlay.navigate_down")});
    QCOMPARE(delivered(DpadLeft, press, open), QStringList{QStringLiteral("overlay.navigate_left")});
    QCOMPARE(delivered(DpadRight, press, open), QStringList{QStringLiteral("overlay.navigate_right")});

    QCOMPARE(delivered(ShoulderLeft, press, open), QStringList{QStringLiteral("overlay.game_prev")});
    QCOMPARE(delivered(ShoulderRight, press, open), QStringList{QStringLiteral("overlay.game_next")});

    // View/Back has no shipped row: its historic capture behaviour is the
    // runtime's capability alias, applied only while the control is unbound —
    // it never adds a second action to an overlay press.
    QCOMPARE(delivered(ViewBack, press, open), QStringList{});
}

// Routing follows the overlay's own visibility, never the OS foreground: with
// the overlay visible the same controls deliver the same overlay actions
// whether or not GameHQ's desktop window holds focus, and only a closed overlay
// hands those controls to the desktop scope. The remembered-game association
// itself is pinned at show time by AppController::syncOverlayToForegroundGame;
// native real-window acceptance stays with cpo-o05.
void OverlayInputTest::desktopFocusCannotRetargetOverlay()
{
    using namespace ControlId;
    const GestureSpec press = GestureSpec::press();

    // Press-activated overlay defaults (the down edge) stay in the overlay's
    // scope whether or not GameHQ's desktop window holds focus.
    const QStringList overlayControls{FaceEast, DpadUp, Menu};
    const QStringList overlayActions{QStringLiteral("overlay.back"),
                                     QStringLiteral("overlay.navigate_up"),
                                     QStringLiteral("overlay.sidebar_toggle")};
    for (int i = 0; i < overlayControls.size(); ++i) {
        QCOMPARE(delivered(overlayControls.at(i), press, Context{true, false, false}),
                 QStringList{overlayActions.at(i)});
        QCOMPARE(delivered(overlayControls.at(i), press, Context{true, false, true}),
                 QStringList{overlayActions.at(i)});
    }

    // Cross is asymmetric on purpose: overlay.confirm fires on the down edge,
    // desktop.confirm is a tap (it shares the button with the bulk hold).
    QCOMPARE(delivered(FaceSouth, press, Context{true, false, false}),
             QStringList{QStringLiteral("overlay.confirm")});
    QCOMPARE(delivered(FaceSouth, press, Context{true, false, true}),
             QStringList{QStringLiteral("overlay.confirm")});
    QCOMPARE(delivered(FaceSouth, GestureSpec::tap(1), Context{false, false, true}),
             QStringList{QStringLiteral("desktop.confirm")});

    // With the overlay closed, the same physical controls are the desktop's.
    QCOMPARE(delivered(FaceEast, press, Context{false, false, true}),
             QStringList{QStringLiteral("desktop.back")});
    QCOMPARE(delivered(DpadUp, press, Context{false, false, true}),
             QStringList{QStringLiteral("desktop.navigate_up")});
    QCOMPARE(delivered(Menu, press, Context{false, false, true}),
             QStringList{QStringLiteral("desktop.settings")});
}

// The escape routes stay reachable in every context, so the user can always
// leave the overlay even though the game keeps receiving the same pad.
void OverlayInputTest::escapeRoutes()
{
    using namespace ControlId;
    const GestureSpec tap = GestureSpec::tap(1);
    const GestureSpec press = GestureSpec::press();

    // PS tap delivers exactly the overlay toggle, whatever else is showing.
    for (bool overlay : {false, true}) {
        for (bool playback : {false, true}) {
            for (bool desktop : {false, true}) {
                QCOMPARE(delivered(Guide, tap, Context{overlay, playback, desktop}),
                         QStringList{QStringLiteral("global.toggle_overlay")});
            }
        }
    }

    // Circle still backs out while a clip is focused (playback is primary, the
    // overlay's own scope is the fallback).
    for (bool playback : {false, true}) {
        QCOMPARE(delivered(FaceEast, press, Context{true, playback, false}),
                 QStringList{QStringLiteral("overlay.back")});
    }

    // The keyboard routes in the shipped table. Ctrl+Shift+G is a Win32 global
    // hotkey, which is why it is the one keyboard escape that works while the
    // game holds focus; Esc/Backspace map to overlay.back but the overlay
    // window is created WindowDoesNotAcceptFocus, so they only act where the
    // overlay itself has keyboard focus.
    bool toggleHotkey = false;
    bool escBack = false;
    bool backspaceBack = false;
    for (const auto& binding : gamehqDefaultBindings()) {
        if (binding.deviceGroup != QLatin1String("keyboard"))
            continue;
        if (binding.triggerCode == QLatin1String("Ctrl+Shift+G")
            && binding.actionId == QLatin1String("global.toggle_overlay"))
            toggleHotkey = true;
        if (binding.actionId == QLatin1String("overlay.back")) {
            if (binding.triggerCode == QLatin1String("Esc"))
                escBack = true;
            if (binding.triggerCode == QLatin1String("Backspace"))
                backspaceBack = true;
        }
    }
    QVERIFY(toggleHotkey);
    QVERIFY(escBack);
    QVERIFY(backspaceBack);
}

// One press cycle, one GameHQ action: every trigger+gesture in the shipped
// controller table delivers at most one action in every context. The one
// shipped pair that would otherwise stack (Share tap during playback) is a
// declared substitution, and the detector itself is proven against a synthetic
// overlap so the audit cannot pass vacuously.
void OverlayInputTest::noDoubleDeliveryInShippedDefaults()
{
    using namespace ControlId;

    QSet<QString> triggerGestureKeys;
    QVector<QPair<QString, GestureSpec>> presses;
    for (const auto& binding : gamehqDefaultBindings()) {
        if (binding.deviceGroup != QLatin1String("controller"))
            continue;
        const GestureSpec gesture = binding.gesture();
        const QString key = binding.triggerCode + QLatin1Char('/')
                            + QString::number(static_cast<int>(gesture.kind))
                            + QLatin1Char('/') + QString::number(gesture.tapCount);
        if (triggerGestureKeys.contains(key))
            continue;
        triggerGestureKeys.insert(key);
        presses.append({binding.triggerCode, gesture});
    }
    QVERIFY2(presses.size() >= 15, "the scan must cover the shipped control set");

    int scans = 0;
    int deliveredScans = 0;
    for (const auto& press : presses) {
        for (bool overlay : {false, true}) {
            for (bool playback : {false, true}) {
                for (bool desktop : {false, true}) {
                    const QStringList actions = delivered(press.first, press.second,
                                                          Context{overlay, playback, desktop});
                    ++scans;
                    if (!actions.isEmpty())
                        ++deliveredScans;
                    QVERIFY2(actions.size() <= 1,
                             qPrintable(QStringLiteral("%1 / %2 delivered %3")
                                            .arg(press.first, press.second.label(),
                                                 actions.join(QStringLiteral(", ")))));
                }
            }
        }
    }
    QVERIFY(scans >= 100);
    QVERIFY(deliveredScans >= 20);

    // The documented substitution: Share tap grabs a frame during playback
    // instead of a screenshot — never both.
    QVERIFY(ContextOverrideCatalog::shadows(QStringLiteral("playback.frame_grab"),
                                            QStringLiteral("global.screenshot"),
                                            GestureSpec::tap(1)));
    QCOMPARE(delivered(Capture, GestureSpec::tap(1), Context{true, true, false}),
             QStringList{QStringLiteral("playback.frame_grab")});

    // And the audit is not vacuous: a user-created second action on one
    // trigger+gesture is a real double delivery the editor is expected to
    // surface, so a scan that returns 2 is a failure, not a silent overlap.
    const QVector<Candidate> stacked{
        {QStringLiteral("overlay.back"), ActionCatalog::Scope::Overlay, GestureSpec::tap(1)},
        {QStringLiteral("global.screenshot"), ActionCatalog::Scope::Global, GestureSpec::tap(1)}};
    const QStringList stackedActions =
        OverlayInput::deliveredActions(stacked, GestureSpec::tap(1), overlayOpen());
    QCOMPARE(stackedActions.size(), 2);
    QVERIFY(stackedActions.contains(QStringLiteral("overlay.back")));
    QVERIFY(stackedActions.contains(QStringLiteral("global.screenshot")));
}

// The overlay shell's half of the contract: left/right (pad or arrow keys) only
// ever seeks a focused clip, and L1/R1 is the only route that moves the strip.
// The real window cannot be instantiated here, so the shipped QML is audited as
// source - the same approach the lifetime suite uses for the shell.
void OverlayInputTest::overlayShellKeepsSeekAndCaptureIndependent()
{
    QFile shell(QStringLiteral(GAMEHQ_SOURCE_DIR "/ui/qml/OverlayWindow.qml"));
    QVERIFY2(shell.open(QIODevice::ReadOnly | QIODevice::Text),
             "OverlayWindow.qml must be readable through GAMEHQ_SOURCE_DIR");
    const QString source = QString::fromUtf8(shell.readAll());

    const QString seek = functionBody(
        source, QStringLiteral("function handleSeekStep(direction) {"));
    QVERIFY2(!seek.isEmpty(), "handleSeekStep must exist in the overlay shell");
    QVERIFY2(seek.contains(QLatin1String("content.videoFocused")),
             "left/right must be gated on a focused clip");
    QVERIFY2(seek.contains(QLatin1String("content.seekVideo(")),
             "a focused clip is still seekable");
    QVERIFY2(!seek.contains(QLatin1String("Capture")),
             "left/right without a focused clip must stay a no-op");

    const QString gameStep = functionBody(
        source, QStringLiteral("function onOverlayGameStep(direction) {"));
    QVERIFY2(!gameStep.isEmpty(), "onOverlayGameStep must exist in the overlay shell");
    QVERIFY2(gameStep.contains(QLatin1String("content.handleCaptureStep(direction)")),
             "L1/R1 remains the capture switch");

    // Exactly one call site each: nothing else in the shell can step the strip.
    QVERIFY2(source.count(QLatin1String("content.handleCaptureStep(")) == 1,
             "the capture step is called from one place only");
    QVERIFY2(source.count(QLatin1String("strip.incrementCurrentIndex()")) == 1,
             "only handleCaptureStep steps the strip forward");
    QVERIFY2(source.count(QLatin1String("strip.decrementCurrentIndex()")) == 1,
             "only handleCaptureStep steps the strip back");
    // The legacy keyboard dual-behaviour handler must not come back.
    QVERIFY(!source.contains(QLatin1String("function handleNavigate(")));
}

QTEST_MAIN(OverlayInputTest)
#include "tst_overlayinput.moc"
