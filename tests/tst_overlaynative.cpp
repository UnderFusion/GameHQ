// cpo-o05: native acceptance of the in-game overlay's window/focus behaviour.
//
// cpo-o06b changed the contract this file pins down. The overlay is no longer
// a window that must never own the foreground: presentation is still
// non-activating, but it is now followed by an explicit, bounded foreground
// request, so while the overlay is open it is expected to BE the active
// window — with the target app still visible and un-minimized behind it, and
// the foreground handed back when the overlay closes.
//
// What this proves, on real Win32 facts rather than fakes:
//   * showing the overlay takes the foreground while the target app (a
//     borderless window in ANOTHER process) stays visible and un-minimized,
//     across repeated open/close cycles, popup interactions and native-handle
//     recreation, and closing gives the foreground back;
//   * a denied foreground request leaves the overlay usable and honest
//     instead of breaking presentation, and never turns into a retry loop;
//   * the overlay closes itself when the foreground genuinely leaves the game
//     (another app activated, target minimized, target window destroyed) and
//     survives a same-process replacement window (game recreates its window);
//   * a stale target handle is never reused after destruction;
//   * the target keeps processing input while the overlay is open, and the
//     GameHQ-side overlay routing (keyed on visibility, not OS focus) still
//     works while the target owns the foreground.
//
// The overlay under test is the shipped one: OverlayWindow.qml, OverlayManager
// and OverlayPresenter are the production sources, loaded through a test-local
// "GameHQ" QML module that embeds the real QML files (tests/CMakeLists.txt).
// The context objects the overlay QML reads (app, input, sounds, overlayGallery,
// languageManager) are stubs with the exact API surface the QML uses — they feed
// content, they do not stand in for the behaviour under test.
//
// What this does NOT claim: physical gamepad delivery needs a real device and
// stays with the controller acceptance work; here the GameHQ-side route is
// driven through the same signals the device layer emits.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <QtTest>
#include <QAbstractListModel>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QProcess>
#include <QScreen>
#include <QUrl>
#include <QVariantMap>

#include <memory>

#include "input/InputDiagnostics.h"
#include "overlay/ForegroundAcquirer.h"
#include "overlay/ForegroundApi.h"
#include "overlay/OverlayManager.h"

#include <windows.h>

#ifndef GAMEHQ_TARGET_FIXTURE
#error "tst_overlaynative needs GAMEHQ_TARGET_FIXTURE (see tests/CMakeLists.txt)"
#endif

namespace
{
// ---------------------------------------------------------------------------
// QML diagnostics: the harness must load the real overlay QML without binding
// breakage, otherwise a green run would prove nothing.
// ---------------------------------------------------------------------------
QStringList g_diagnostics;
bool g_captureDiagnostics = false;

void diagnosticHandler(QtMsgType type, const QMessageLogContext&, const QString& message)
{
    if (g_captureDiagnostics)
        g_diagnostics.append(message);
    if (type != QtMsgType::QtDebugMsg)
        fprintf(stderr, "%s\n", qPrintable(message));
}

QStringList brokenQmlDiagnostics()
{
    const QStringList patterns = { QStringLiteral("ReferenceError"),
                                   QStringLiteral("is not a type"),
                                   QStringLiteral("Unable to assign"),
                                   QStringLiteral("TypeError"),
                                   QStringLiteral("failed to load component"),
                                   QStringLiteral("Cannot assign") };
    QStringList broken;
    for (const QString& message : g_diagnostics)
        for (const QString& pattern : patterns)
            if (message.contains(pattern))
                broken.append(message);
    return broken;
}

// ---------------------------------------------------------------------------
// Win32 helpers
// ---------------------------------------------------------------------------
QString windowTitleOf(HWND hwnd)
{
    wchar_t buffer[256]{};
    const int length = GetWindowTextW(hwnd, buffer, 256);
    return QString::fromWCharArray(buffer, length);
}

struct TitleSearch
{
    QString prefix;
    HWND found = nullptr;
};

QString windowSnapshot(HWND hwnd)
{
    if (!hwnd)
        return QStringLiteral("0x0 (none)");
    wchar_t className[128]{};
    GetClassNameW(hwnd, className, 128);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return QStringLiteral("0x%1 \"%2\" class=%3 pid=%4 visible=%5 iconic=%6")
        .arg(QString::number(reinterpret_cast<qulonglong>(hwnd), 16))
        .arg(windowTitleOf(hwnd))
        .arg(QString::fromWCharArray(className))
        .arg(pid)
        .arg(IsWindowVisible(hwnd) ? 1 : 0)
        .arg(IsIconic(hwnd) ? 1 : 0);
}

BOOL CALLBACK titleSearchProc(HWND hwnd, LPARAM param)
{
    auto* search = reinterpret_cast<TitleSearch*>(param);
    if (windowTitleOf(hwnd).startsWith(search->prefix)) {
        search->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND windowWithTitlePrefix(const QString& prefix)
{
    TitleSearch search;
    search.prefix = prefix;
    EnumWindows(titleSearchProc, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool hasNoActivateStyle(HWND hwnd)
{
    return hwnd && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0;
}

// Counts what the overlay ASKED Windows to do, and optionally refuses it.
// Windows denying the foreground cannot be provoked on a live desktop, and
// "the overlay never requested the foreground" cannot be observed from the
// outside at all — both need this seam. Ownership passes to the manager.
class CountingForegroundApi final : public ForegroundApi
{
public:
    explicit CountingForegroundApi(bool allow)
        : m_allow(allow)
        , m_system(ForegroundApi::createSystem())
    {
    }

    void* foregroundWindow() override { return m_system->foregroundWindow(); }

    bool forceForeground(void* target) override
    {
        ++m_attempts;
        return m_allow && m_system->forceForeground(target);
    }

    int attempts() const { return m_attempts; }

private:
    bool m_allow;
    std::unique_ptr<ForegroundApi> m_system;
    int m_attempts = 0;
};

// The command message the fixture registers. RegisterWindowMessage returns the
// same value in every process on the desktop, so the fixture and this test
// agree without shipping a header for it.
UINT fixtureCommandMessage()
{
    static const UINT id = RegisterWindowMessageW(L"GameHQ.OverlayNativeFixture.Command");
    return id;
}

enum FixtureCommand : WPARAM {
    CmdRecreate = 1,
    CmdDestroyWindow = 2,
    CmdMinimize = 3,
    CmdRestoreForeground = 4,
    CmdPing = 5,
    CmdQuit = 6,
};

// ---------------------------------------------------------------------------
// Stubs for the context objects the overlay QML reads. The member list is the
// full set the QML closure touches (audited against src/ui/qml).
// ---------------------------------------------------------------------------
class StubAppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList games READ games CONSTANT)
    Q_PROPERTY(QString currentGameId READ currentGameId CONSTANT)
    Q_PROPERTY(bool currentGameAvailable READ currentGameAvailable CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)

public:
    using QObject::QObject;

    QVariantList games() const
    {
        QVariantList list;
        list.append(QVariantMap{ { QStringLiteral("id"), QStringLiteral("fixture-game") },
                                 { QStringLiteral("name"), QStringLiteral("Fixture Game") } });
        return list;
    }
    QString currentGameId() const { return QString(); }
    bool currentGameAvailable() const { return false; }
    QString version() const { return QStringLiteral("0.0.0-native"); }

    // The overlay remembers the category per game through this call.
    Q_INVOKABLE QString overlayCategory(int) const { return QStringLiteral("all"); }

    Q_INVOKABLE QVariant config(const QString&, const QVariant& fallback) const { return fallback; }
    Q_INVOKABLE void setOverlayCategory(const QString&, const QString&) {}
    Q_INVOKABLE void deleteCaptureFrom(QObject*, int) {}
    Q_INVOKABLE void saveVideoFrame(QObject*, const QString&) {}
    Q_INVOKABLE void showInFolderFrom(QObject*, int) {}

signals:
    void currentGameChanged();
    void configChanged(const QString& key, const QVariant& value);
    void configGroupReset(const QString& prefix);
};

class StubGalleryModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        ThumbnailRole = Qt::UserRole + 1,
        CaptureTypeRole,
        GameNameRole,
        DateTextRole,
        FavoriteRole,
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : 3;
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() > 2)
            return QVariant();
        const bool isVideo = index.row() < 2;
        switch (role) {
        case ThumbnailRole:
            return QString();
        case CaptureTypeRole:
            return isVideo ? QStringLiteral("video") : QStringLiteral("screenshot");
        case GameNameRole:
            return QStringLiteral("Fixture Game");
        case DateTextRole:
            return QStringLiteral("2026-09-20 12:0%1").arg(index.row());
        case FavoriteRole:
            return false;
        default:
            return QVariant();
        }
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return { { ThumbnailRole, "thumbnail" },
                 { CaptureTypeRole, "captureType" },
                 { GameNameRole, "gameName" },
                 { DateTextRole, "dateText" },
                 { FavoriteRole, "favorite" } };
    }

    Q_INVOKABLE void setFilter(const QString&, int = -1) {}
    Q_INVOKABLE void toggleFavorite(int) {}

    Q_INVOKABLE QVariantMap get(int row) const
    {
        if (row < 0 || row > 2)
            return QVariantMap();
        const bool isVideo = row < 2;
        QVariantMap record;
        record.insert(QStringLiteral("fileUrl"), QVariant());
        record.insert(QStringLiteral("captureType"),
                      isVideo ? QStringLiteral("video") : QStringLiteral("screenshot"));
        record.insert(QStringLiteral("gameName"), QStringLiteral("Fixture Game"));
        record.insert(QStringLiteral("dateText"), QStringLiteral("2026-09-20 12:0%1").arg(row));
        record.insert(QStringLiteral("thumbnail"), QString());
        record.insert(QStringLiteral("favorite"), false);
        return record;
    }
};

class StubInput : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    Q_INVOKABLE bool handleKeyPressed(int, int, bool = false) { return false; }
    Q_INVOKABLE bool handleKeyReleased(int, int) { return false; }
    Q_INVOKABLE void setPlaybackActive(bool) {}

signals:
    void overlayNavigate(int direction);
    void overlayNavigateVertical(int direction);
    void overlayConfirm();
    void overlayFavorite();
    void overlayMenu();
    void overlayGameStep(int direction);
    void overlayHideRequested();
    void playbackPlayPause();
    void playbackSeek(int direction);
    void frameGrabRequested();
};

class StubSounds : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE void play(const QString&) {}
};

class StubLanguageManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Qt::LayoutDirection layoutDirection READ layoutDirection CONSTANT)
    Q_PROPERTY(int translationRevision READ translationRevision CONSTANT)
public:
    using QObject::QObject;
    Qt::LayoutDirection layoutDirection() const { return Qt::LeftToRight; }
    int translationRevision() const { return 0; }
    Q_INVOKABLE QString formatDuration(int) const { return QStringLiteral("0:00"); }
};

// ---------------------------------------------------------------------------
// The fixture process: a borderless "game" window in another process.
// ---------------------------------------------------------------------------
class FixtureTarget
{
public:
    ~FixtureTarget() { stop(); }

    bool start()
    {
        m_process.setProgram(QString::fromUtf8(GAMEHQ_TARGET_FIXTURE));
        m_process.start();
        if (!m_process.waitForStarted(10000))
            return false;
        const QString id = QString::number(m_process.processId());
        m_control = waitForWindow(QStringLiteral("GameHQOverlayControl#") + id, 10000);
        if (!m_control)
            return false;
        m_targetPrefix = QStringLiteral("GameHQOverlayTarget#") + id + QLatin1Char('#');
        return refreshTarget(10000);
    }

    bool isRunning() const { return m_process.state() != QProcess::NotRunning; }
    HWND control() const { return m_control; }
    HWND target() const { return m_target; }

    bool sendToControl(WPARAM command) const
    {
        return m_control && PostMessageW(m_control, fixtureCommandMessage(), command, 0);
    }

    bool pingTarget() const
    {
        return m_target && PostMessageW(m_target, fixtureCommandMessage(), CmdPing, 0);
    }

    bool recreate()
    {
        const int previous = generation();
        // The replacement is the fixture's answer to the command, not ours:
        // forget the cached handle and wait for a window whose generation is
        // actually new instead of accepting the not-yet-destroyed old one.
        m_target = nullptr;
        if (!sendToControl(CmdRecreate))
            return false;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            const HWND found = windowWithTitlePrefix(m_targetPrefix);
            if (found) {
                m_target = found;
                if (generation() != previous)
                    return true;
            }
            QTest::qWait(25);
        }
        return false;
    }

    bool destroyTargetWindow()
    {
        if (!sendToControl(CmdDestroyWindow))
            return false;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            if (!windowWithTitlePrefix(m_targetPrefix)) {
                m_target = nullptr;
                return true;
            }
            QTest::qWait(25);
        }
        return false;
    }

    bool minimizeTarget() const { return sendToControl(CmdMinimize); }

    bool forceTargetForeground()
    {
        if (!refreshTarget(1000))
            return false;
        std::unique_ptr<ForegroundApi> api(ForegroundApi::createSystem());
        for (int attempt = 0; attempt < 5; ++attempt) {
            api->forceForeground(m_target);
            for (int wait = 0; wait < 20; ++wait) {
                if (GetForegroundWindow() == m_target)
                    return true;
                QTest::qWait(25);
            }
        }
        return GetForegroundWindow() == m_target;
    }

    int generation() const { return parseTitlePart(2); }
    unsigned pings() const { return static_cast<unsigned>(parseTitlePart(3)); }

    void stop()
    {
        if (!isRunning())
            return;
        sendToControl(CmdQuit);
        if (!m_process.waitForFinished(5000)) {
            m_process.kill();
            m_process.waitForFinished(2000);
        }
    }

private:
    HWND waitForWindow(const QString& prefix, int timeoutMs)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            if (HWND window = windowWithTitlePrefix(prefix))
                return window;
            QTest::qWait(25);
        }
        return nullptr;
    }

    bool refreshTarget(int timeoutMs)
    {
        const HWND found = waitForWindow(m_targetPrefix, timeoutMs);
        if (!found)
            return false;
        m_target = found;
        return true;
    }

    // Title: GameHQOverlayTarget#<pid>#<generation>#p<pings>
    int parseTitlePart(int index) const
    {
        if (!m_target)
            return 0;
        const QStringList parts = windowTitleOf(m_target).split(QLatin1Char('#'));
        if (index >= parts.size())
            return 0;
        QString value = parts.at(index);
        if (value.startsWith(QLatin1Char('p')))
            value = value.mid(1);
        return value.toInt();
    }

    QProcess m_process;
    HWND m_control = nullptr;
    HWND m_target = nullptr;
    QString m_targetPrefix;
};

// ---------------------------------------------------------------------------
// The overlay under test: shipped manager + presenter + QML.
// ---------------------------------------------------------------------------
struct OverlayHarness
{
    // Declaration order is destruction order, reversed: the stubs must outlive
    // the engine, because the loaded QML binds to them until it is destroyed.
    StubAppController app;
    StubGalleryModel gallery;
    StubInput input;
    StubSounds sounds;
    StubLanguageManager language;
    std::unique_ptr<OverlayManager> manager;
    QQmlApplicationEngine engine;

    QQuickWindow* window() const
    {
        for (QObject* root : engine.rootObjects()) {
            if (auto* quickWindow = qobject_cast<QQuickWindow*>(root))
                return quickWindow;
        }
        return nullptr;
    }

    HWND handle() const
    {
        QQuickWindow* quickWindow = window();
        return quickWindow ? reinterpret_cast<HWND>(quickWindow->winId()) : nullptr;
    }
};

QQuickItem* findItemByProperty(QQuickItem* root, const char* propertyName)
{
    if (!root)
        return nullptr;
    if (root->metaObject()->indexOfProperty(propertyName) >= 0)
        return root;
    const QList<QQuickItem*> children = root->childItems();
    for (QQuickItem* child : children) {
        if (QQuickItem* found = findItemByProperty(child, propertyName))
            return found;
    }
    return nullptr;
}

QQuickItem* findItemByClass(QQuickItem* root, const char* classNamePart)
{
    if (!root)
        return nullptr;
    if (QString::fromLatin1(root->metaObject()->className()).contains(QLatin1String(classNamePart)))
        return root;
    const QList<QQuickItem*> children = root->childItems();
    for (QQuickItem* child : children) {
        if (QQuickItem* found = findItemByClass(child, classNamePart))
            return found;
    }
    return nullptr;
}

}  // namespace

class NativeOverlayTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void openingTheOverlayTakesForegroundWhileTheTargetStaysVisible();
    void theTargetStaysTheGameContextWhileTheOverlayIsActive();
    void aDeniedForegroundRequestLeavesTheOverlayUsable();
    void realShowAndHideLandInTheOverlayDiagnostics();
    void repeatedOpenCloseCyclesHandTheForegroundBackAndForth();
    void menuAndDeleteConfirmationNeverActivateAnotherWindow();
    void anotherApplicationTakingTheForegroundDismissesTheOverlay();
    void minimizingTheTargetDismissesTheOverlay();
    void targetWindowRecreationKeepsTheOverlayOpen();
    void nativeHandleRecreationIsRepairedByTheShowPath();
    void destroyedTargetHandlesAreNeverReused();
    void targetKeepsWorkingWhileTheOverlayIsOpen();
    void overlayFollowsAGameOnAnotherScreenWhenOneExists();

private:
    // `foregroundApi` (optional, ownership passes on) lets a case decide what
    // Windows does with the overlay's foreground request.
    std::unique_ptr<OverlayHarness> makeHarness(ForegroundApi* foregroundApi = nullptr) const
    {
        auto harness = std::make_unique<OverlayHarness>();
        // Qt maps module URIs onto the resource tree; make that explicit so the
        // test module is found regardless of the engine's default import paths.
        harness->engine.addImportPath(QStringLiteral(":/qt/qml"));
        harness->engine.rootContext()->setContextProperty(QStringLiteral("app"), &harness->app);
        harness->engine.rootContext()->setContextProperty(QStringLiteral("overlayGallery"),
                                                          &harness->gallery);
        harness->engine.rootContext()->setContextProperty(QStringLiteral("input"), &harness->input);
        harness->engine.rootContext()->setContextProperty(QStringLiteral("sounds"), &harness->sounds);
        harness->engine.rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                                          &harness->language);
        harness->manager = std::make_unique<OverlayManager>(&harness->engine, foregroundApi);
        harness->engine.rootContext()->setContextProperty(QStringLiteral("overlay"),
                                                          harness->manager.get());
        return harness;
    }

    void showOverlay(OverlayHarness& harness)
    {
        harness.manager->show();
        QTRY_VERIFY_WITH_TIMEOUT(harness.window() != nullptr, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(harness.manager->isVisible(), 5000);
    }

    // A failed foreground check must say what actually holds the foreground:
    // on a live desktop "something else took focus" is exactly the diagnosis
    // that separates a product defect from environment interference.
    //
    // Retried rather than sampled once: handing the foreground back on close is
    // a bounded request with its own retries, so the assertion has to allow the
    // same budget the product does.
    void expectTargetForeground()
    {
        QTRY_VERIFY2_WITH_TIMEOUT(
            GetForegroundWindow() == m_fixture.target(),
            qPrintable(QStringLiteral("foreground is %1, expected the fixture target %2")
                           .arg(windowSnapshot(GetForegroundWindow()),
                                windowSnapshot(m_fixture.target()))),
            3000);
    }

    // cpo-o06b's acceptance shape, as one call: the overlay owns the
    // foreground AND the target is still on screen behind it. Both halves
    // matter — a game that minimized itself in reaction would leave the first
    // one true and the feature broken.
    void expectOverlayForeground(OverlayHarness& harness)
    {
        const HWND overlay = harness.handle();
        QVERIFY(overlay != nullptr);
        QTRY_VERIFY2_WITH_TIMEOUT(
            GetForegroundWindow() == overlay,
            qPrintable(QStringLiteral("foreground is %1, expected the overlay %2")
                           .arg(windowSnapshot(GetForegroundWindow()),
                                windowSnapshot(overlay))),
            3000);
        expectTargetStillOnScreen();
        QVERIFY2(harness.manager->isVisible(),
                 "the overlay dismissed itself after taking the foreground");
        QVERIFY2(!hasNoActivateStyle(overlay),
                 "an interactive overlay must not keep WS_EX_NOACTIVATE");
    }

    void expectTargetStillOnScreen()
    {
        const HWND target = m_fixture.target();
        QVERIFY2(target && IsWindow(target),
                 "the target window disappeared while the overlay was open");
        QVERIFY2(IsWindowVisible(target),
                 qPrintable(QStringLiteral("the target is not visible: %1")
                                .arg(windowSnapshot(target))));
        QVERIFY2(!IsIconic(target),
                 qPrintable(QStringLiteral("the target minimized itself: %1")
                                .arg(windowSnapshot(target))));
    }

    FixtureTarget m_fixture;
};

void NativeOverlayTest::initTestCase()
{
    qInstallMessageHandler(diagnosticHandler);
    g_captureDiagnostics = true;
    QVERIFY2(m_fixture.start(), "the fixture process did not come up");
    QVERIFY(m_fixture.control() != nullptr);
    QVERIFY(m_fixture.target() != nullptr);
}

void NativeOverlayTest::cleanupTestCase()
{
    m_fixture.stop();
    qInstallMessageHandler(nullptr);
}

void NativeOverlayTest::openingTheOverlayTakesForegroundWhileTheTargetStaysVisible()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");

    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    g_diagnostics.clear();
    auto harness = makeHarness();
    showOverlay(*harness);

    const HWND overlay = harness->handle();
    QVERIFY(overlay != nullptr);
    QVERIFY(IsWindowVisible(overlay));

    // The whole point of this leaf: the overlay is the active window, the
    // target is still on screen and un-minimized behind it.
    expectOverlayForeground(*harness);
    QVERIFY((GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

    // ... and it stays that way. Taking the foreground away from the game is
    // exactly the event the lifetime rules used to read as "the user left":
    // if that misfired, the overlay would be gone by now.
    QTest::qWait(1200);
    QVERIFY2(harness->manager->isVisible(),
             "the overlay dismissed itself on its own expected foreground");
    QCOMPARE(GetForegroundWindow(), overlay);
    expectTargetStillOnScreen();

    // The overlay covers the screen the target is on (single-screen session,
    // so that is the primary screen).
    QCOMPARE(harness->window()->geometry(), QGuiApplication::primaryScreen()->geometry());

    QCOMPARE(brokenQmlDiagnostics(), QStringList());
    harness->manager->hide();
    expectTargetForeground();
}

// The game the overlay opened over stays the game context even once GameHQ
// owns the OS foreground: hideForDesktopHandoff() returns the window the
// manager remembers, so a manager that had let its own window become "the
// game" would hand back the wrong handle — or none.
void NativeOverlayTest::theTargetStaysTheGameContextWhileTheOverlayIsActive()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    auto* counting = new CountingForegroundApi(true);
    auto harness = makeHarness(counting);
    showOverlay(*harness);
    expectOverlayForeground(*harness);
    const int afterShow = counting->attempts();
    QVERIFY(afterShow > 0);

    const HWND remembered = static_cast<HWND>(harness->manager->hideForDesktopHandoff());
    QCOMPARE(remembered, m_fixture.target());
    QTRY_VERIFY_WITH_TIMEOUT(!harness->manager->isVisible(), 5000);

    // The desktop handoff deliberately does NOT ask for the foreground back:
    // GameHQ's main window is about to take it, and two windows requesting it
    // is the fight this avoids. Windows is free to promote whatever it likes
    // once the overlay hides — what is asserted here is that GameHQ made no
    // request of its own.
    QTest::qWait(400);
    QCOMPARE(counting->attempts(), afterShow);
}

// A denied foreground request must not cost anything: the overlay is still
// presented, still open and still honest about the game keeping the input.
void NativeOverlayTest::aDeniedForegroundRequestLeavesTheOverlayUsable()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    auto* refusing = new CountingForegroundApi(false);
    auto harness = makeHarness(refusing);
    showOverlay(*harness);

    const HWND overlay = harness->handle();
    QVERIFY(overlay != nullptr);
    QVERIFY(IsWindowVisible(overlay));
    QVERIFY((GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

    // Presentation is untouched by the denial, the overlay stays open, and the
    // target keeps the foreground exactly as it did before cpo-o06b.
    QTest::qWait(600);
    QVERIFY2(harness->manager->isVisible(), "a denied foreground request closed the overlay");
    expectTargetForeground();
    expectTargetStillOnScreen();
    QVERIFY2(!harness->manager->foregroundAcquired(),
             "a denied request must not be reported as acquired");

    // Bounded, not a loop: the acquirer spends its fixed budget and stops.
    // Anything else would be GameHQ fighting the shell for focus.
    const int afterShow = refusing->attempts();
    QCOMPARE(afterShow, ForegroundAcquirer::kMaxAttempts);
    QTest::qWait(700);
    QCOMPARE(refusing->attempts(), afterShow);

    harness->manager->hide();
    QTest::qWait(100);
    // Nothing was taken, so nothing is handed back: no second acquisition.
    QCOMPARE(refusing->attempts(), afterShow);
    expectTargetForeground();
}

// cpo-x01: the advertised overlay show/hide timeline must come from the real
// show/hide path, not from a test-seeded ring. The environment decides whether
// the game keeps the foreground (yes and no are both acceptable here); what
// must never happen is an overlay entry that reports nothing observed.
void NativeOverlayTest::realShowAndHideLandInTheOverlayDiagnostics()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");

    InputDiagnostics::instance().clear();
    auto harness = makeHarness();
    showOverlay(*harness);

    QString text = InputDiagnostics::instance().exportBetaText(
        QStringLiteral("build"), QStringLiteral("windows"), {}, {});
    QVERIFY2(text.contains(QStringLiteral("overlay show | ")), qPrintable(text));
    QVERIFY2(!text.contains(QStringLiteral("unavailable (no overlay show this session)")),
             qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("game foreground preserved on show: yes"))
                 || text.contains(QStringLiteral("game foreground preserved on show: no")),
             qPrintable(text));

    harness->manager->hide();
    QTRY_VERIFY_WITH_TIMEOUT(!harness->manager->isVisible(), 5000);
    text = InputDiagnostics::instance().exportBetaText(
        QStringLiteral("build"), QStringLiteral("windows"), {}, {});
    QVERIFY2(text.contains(QStringLiteral("overlay hide")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("overlay visible: no")), qPrintable(text));
}

void NativeOverlayTest::repeatedOpenCloseCyclesHandTheForegroundBackAndForth()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    auto harness = makeHarness();
    for (int cycle = 0; cycle < 5; ++cycle) {
        harness->manager->toggle();
        QTRY_VERIFY_WITH_TIMEOUT(harness->manager->isVisible(), 5000);
        expectOverlayForeground(*harness);

        harness->manager->toggle();
        QTRY_VERIFY_WITH_TIMEOUT(!harness->manager->isVisible(), 5000);
        // Closing gives the foreground back to the window the overlay took it
        // from — every cycle, not just the first.
        expectTargetForeground();
        expectTargetStillOnScreen();
    }
}

void NativeOverlayTest::menuAndDeleteConfirmationNeverActivateAnotherWindow()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    g_diagnostics.clear();
    auto harness = makeHarness();
    showOverlay(*harness);
    expectOverlayForeground(*harness);

    QQuickWindow* window = harness->window();
    QQuickItem* content = findItemByProperty(window->contentItem(), "menuOpen");
    QVERIFY2(content, "the overlay's content item was not found");

    // Real per-capture action menu (Square / M path).
    QVERIFY(QMetaObject::invokeMethod(content, "toggleMenu"));
    QTRY_VERIFY_WITH_TIMEOUT(content->property("menuOpen").toBool(), 2000);
    QTest::qWait(150);
    // The popups belong to the overlay: they must not move the foreground to
    // any other window — not to a popup of their own, and not back to the game.
    expectOverlayForeground(*harness);

    // Real delete confirmation (the mouse delete path opens this dialog).
    QQuickItem* dialog = findItemByClass(window->contentItem(), "ConfirmDialog");
    QVERIFY2(dialog, "the overlay's confirm dialog was not found");
    window->setProperty("pendingDeleteRow", 0);
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY_WITH_TIMEOUT(dialog->property("visible").toBool(), 2000);
    QTest::qWait(150);
    // The popups belong to the overlay: they must not move the foreground to
    // any other window — not to a popup of their own, and not back to the game.
    expectOverlayForeground(*harness);

    // Closing both again must not activate anything either.
    QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
    QTRY_VERIFY_WITH_TIMEOUT(!dialog->property("visible").toBool(), 2000);
    QVERIFY(QMetaObject::invokeMethod(content, "toggleMenu"));
    QTRY_VERIFY_WITH_TIMEOUT(!content->property("menuOpen").toBool(), 2000);
    expectOverlayForeground(*harness);
    QCOMPARE(brokenQmlDiagnostics(), QStringList());

    harness->manager->hide();
    expectTargetForeground();
}

void NativeOverlayTest::anotherApplicationTakingTheForegroundDismissesTheOverlay()
{
    // A second fixture process is "another application" (Alt-Tab / Start menu /
    // a click on another app all arrive as the same OS event).
    FixtureTarget other;
    QVERIFY(other.start());

    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");

    auto harness = makeHarness();
    showOverlay(*harness);
    expectOverlayForeground(*harness);

    // The user moving to a genuinely unrelated window still closes the
    // overlay — and the overlay must NOT pull the foreground back to the game
    // on the way out, which would be focus ping-pong with the user.
    QVERIFY(other.forceTargetForeground());
    QTRY_VERIFY_WITH_TIMEOUT(!harness->manager->isVisible(), 5000);
    QVERIFY2(GetForegroundWindow() == other.target(),
             qPrintable(QStringLiteral("foreground is %1, expected the second fixture %2")
                            .arg(windowSnapshot(GetForegroundWindow()),
                                 windowSnapshot(other.target()))));
}

void NativeOverlayTest::minimizingTheTargetDismissesTheOverlay()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    auto harness = makeHarness();
    showOverlay(*harness);
    expectOverlayForeground(*harness);

    // No foreground event can report this: the overlay already owns the
    // foreground, so the game minimizing changes nothing Windows notifies
    // about. The polled game-context watch is what has to catch it.
    QVERIFY(m_fixture.minimizeTarget());
    QTRY_VERIFY_WITH_TIMEOUT(IsIconic(m_fixture.target()), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness->manager->isVisible(), 5000);
}

void NativeOverlayTest::targetWindowRecreationKeepsTheOverlayOpen()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    auto harness = makeHarness();
    showOverlay(*harness);
    expectOverlayForeground(*harness);

    // The game replaces its own window (borderless toggle, resolution change,
    // launcher handover): the process stays, the handle does not.
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    // The overlay must still be there — the pid proves it is the same game.
    QTest::qWait(600);
    QVERIFY2(harness->manager->isVisible(),
             "the overlay closed on a same-process window replacement");
    // The replacement window took the foreground itself; GameHQ does not
    // fight it back. The overlay stays open over the rebound game.
    expectTargetForeground();
    QVERIFY2(!hasNoActivateStyle(harness->handle()),
             "the rebind path must keep the interactive mode it was in");

    harness->manager->hide();
    expectTargetForeground();
}

void NativeOverlayTest::nativeHandleRecreationIsRepairedByTheShowPath()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    auto harness = makeHarness();
    showOverlay(*harness);
    const HWND before = harness->handle();
    expectOverlayForeground(*harness);

    // Qt rebuilds the native window through exactly this API on flag, screen
    // and geometry transitions; a rebuilt window starts without our ex-style.
    QQuickWindow* window = harness->window();
    window->destroy();
    QTest::qWait(100);
    window->show();
    QTRY_VERIFY_WITH_TIMEOUT(window->winId() != 0, 3000);

    // Drive the shipped repair path: the next production present must restore
    // the guarantee on whatever handle exists now, without activating.
    harness->manager->toggle();
    QTRY_VERIFY_WITH_TIMEOUT(!harness->manager->isVisible(), 3000);
    harness->manager->toggle();
    QTRY_VERIFY_WITH_TIMEOUT(harness->manager->isVisible(), 5000);

    const HWND after = harness->handle();
    QVERIFY(after != nullptr);
    QVERIFY2(after != before, "the native window was not actually rebuilt");
    // The repair path re-applies whatever mode the presentation is in on the
    // NEW handle: a rebuilt window that kept the old style would be either
    // permanently unactivatable or permanently activatable.
    expectOverlayForeground(*harness);

    harness->manager->hide();
    expectTargetForeground();
}

void NativeOverlayTest::destroyedTargetHandlesAreNeverReused()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    auto harness = makeHarness();
    showOverlay(*harness);
    expectOverlayForeground(*harness);

    const HWND gone = m_fixture.target();
    QVERIFY(m_fixture.destroyTargetWindow());
    QVERIFY(!IsWindow(gone));

    // A destroyed remembered window is a real loss of context: the overlay
    // closes instead of measuring or following a dead handle.
    QTRY_VERIFY_WITH_TIMEOUT(!harness->manager->isVisible(), 5000);

    // Foreground churn while closed must stay harmless, and the next target
    // window (same process, new handle) must work with a fresh overlay show.
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());
    showOverlay(*harness);
    expectOverlayForeground(*harness);

    harness->manager->hide();
    expectTargetForeground();
}

void NativeOverlayTest::targetKeepsWorkingWhileTheOverlayIsOpen()
{
    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    auto harness = makeHarness();
    showOverlay(*harness);
    expectOverlayForeground(*harness);

    // The target still receives and processes its window messages while the
    // overlay covers the screen and owns the foreground: losing the
    // foreground is not the same as being frozen out.
    const unsigned pingsBefore = m_fixture.pings();
    QVERIFY(m_fixture.pingTarget());
    QTRY_COMPARE_WITH_TIMEOUT(m_fixture.pings(), pingsBefore + 1, 3000);
    expectOverlayForeground(*harness);

    // And the GameHQ-side overlay route keeps working while the target owns
    // the foreground: it is keyed on overlay visibility, not on OS focus
    // (cpo-o04). The device layer's signals are the ones the pad would emit.
    QQuickItem* content = findItemByProperty(harness->window()->contentItem(), "menuOpen");
    QVERIFY(content);
    QVERIFY(!content->property("menuOpen").toBool());
    QVERIFY(QMetaObject::invokeMethod(&harness->input, "overlayMenu"));
    QTRY_VERIFY_WITH_TIMEOUT(content->property("menuOpen").toBool(), 2000);
    expectOverlayForeground(*harness);

    // Back/Circle inside an open menu closes the menu first; only the next
    // Back closes the overlay itself (the shipped shell contract).
    QVERIFY(QMetaObject::invokeMethod(&harness->input, "overlayHideRequested"));
    QTRY_VERIFY_WITH_TIMEOUT(!content->property("menuOpen").toBool(), 2000);
    expectOverlayForeground(*harness);

    QVERIFY(QMetaObject::invokeMethod(&harness->input, "overlayHideRequested"));
    QTRY_VERIFY_WITH_TIMEOUT(!harness->manager->isVisible(), 5000);
    expectTargetForeground();
    expectTargetStillOnScreen();
}

void NativeOverlayTest::overlayFollowsAGameOnAnotherScreenWhenOneExists()
{
    if (QGuiApplication::screens().size() < 2)
        QSKIP("this machine has a single screen: the cross-screen move needs two");

    if (!m_fixture.forceTargetForeground())
        QSKIP("this session cannot put the fixture window in the foreground");
    QVERIFY(m_fixture.recreate());
    QVERIFY(m_fixture.forceTargetForeground());

    auto harness = makeHarness();
    showOverlay(*harness);
    QScreen* start = harness->window()->screen();
    QVERIFY(start);

    QScreen* other = nullptr;
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != start) {
            other = screen;
            break;
        }
    }
    QVERIFY(other);

    // The game comes back on another monitor after replacing its window (the
    // launcher-handover shape): the pid still proves it is the same game, and
    // the overlay has to follow it through the presenter — rebind plus
    // reposition, never an activation.
    QVERIFY(m_fixture.recreate());
    const QRect area = other->geometry();
    QVERIFY(SetWindowPos(m_fixture.target(), nullptr, area.x() + 60, area.y() + 60, 640, 480,
                         SWP_NOZORDER | SWP_NOACTIVATE));
    QVERIFY(m_fixture.forceTargetForeground());

    QTRY_VERIFY_WITH_TIMEOUT(harness->window()->screen() == other, 5000);
    expectTargetForeground();
    QVERIFY2(!hasNoActivateStyle(harness->handle()),
             "following the game to another monitor must keep the interactive mode");

    harness->manager->hide();
    expectTargetForeground();
}

QTEST_MAIN(NativeOverlayTest)
#include "tst_overlaynative.moc"
