#include <QtTest>
#include <QTranslator>
#include <QScopeGuard>
#include <future>
#include <thread>

#include "input/HotkeyManager.h"

#include <windows.h>

// 0.7.3 hardening regression for HotkeyManager::dispatch: it used to keep a
// reference into m_bindings across its synchronous signal emits. A slot that
// clears or rebinds the very binding being dispatched invalidates that
// reference (QHash erase/rehash), so the final hotkeyTriggered emit could read
// freed memory. dispatch now copies the action id before the first emit.
//
// WM_HOTKEY is synthesized straight into nativeEventFilter — no keyboard
// injection, so the test is deterministic. The chords are real RegisterHotKey
// registrations, chosen deliberately exotic (Ctrl+Alt+Shift+F22..F24); if
// another process happens to own one, the test skips rather than fails.
class HotkeyReentrancyTest : public QObject
{
    Q_OBJECT

    // Deliver WM_HOTKEY for whatever id the manager registered. Ids are
    // assigned internally, so probe the small id space instead of peeking
    // at private state; the filter returns false for unknown ids.
    static bool fire(HotkeyManager& hotkeys)
    {
        for (int id = 1; id <= 32; ++id) {
            MSG msg = {};
            msg.message = WM_HOTKEY;
            msg.wParam = static_cast<WPARAM>(id);
            qintptr result = 0;
            if (hotkeys.nativeEventFilter(QByteArrayLiteral("windows_generic_MSG"),
                                          &msg, &result))
                return true;
        }
        return false;
    }

private slots:
    void unavailablePresetChordPreservesItsUnclaimedRegistration_data()
    {
        QTest::addColumn<bool>("transfer");
        QTest::addColumn<bool>("transferFirst");
        QTest::newRow("retain-current") << false << false;
        QTest::newRow("transfer-after-rejection") << true << false;
        QTest::newRow("transfer-before-rejection") << true << true;
    }

    void unavailablePresetChordPreservesItsUnclaimedRegistration()
    {
        QFETCH(bool, transfer);
        QFETCH(bool, transferFirst);
        HotkeyManager hotkeys;
        const QString screenshot = QStringLiteral("global.screenshot");
        const QString replay = QStringLiteral("global.save_replay");
        const auto oldChord = HotkeyManager::parseChord("Ctrl+Alt+Shift+F9");
        const auto busyChord = HotkeyManager::parseChord("Ctrl+Alt+Shift+F10");
        if (!hotkeys.applyBindingSlot(screenshot, 1, oldChord.modifiers, oldChord.vk))
            QSKIP("Test F9 chord already in use");

        // A different Windows thread keeps F10 reserved throughout the switch.
        std::promise<bool> ready;
        std::promise<void> release;
        std::thread peer([&] {
            const bool registered = RegisterHotKey(nullptr, 1, busyChord.modifiers, busyChord.vk);
            ready.set_value(registered);
            release.get_future().wait();
            if (registered)
                UnregisterHotKey(nullptr, 1);
        });
        const auto releaseReservation = [&] {
            if (peer.joinable()) {
                release.set_value();
                peer.join();
            }
        };
        const auto stopPeer = qScopeGuard(releaseReservation);
        if (!ready.get_future().get())
            QSKIP("Test F10 chord already in use");

        QList<HotkeyManager::Slot> desired{{screenshot, 1, busyChord}};
        if (transfer)
            desired.insert(transferFirst ? 0 : 1, {replay, 2, oldChord});
        hotkeys.synchronize(desired);
        QSignalSpy triggered(&hotkeys, &HotkeyManager::hotkeyTriggered);
        QVERIFY(fire(hotkeys));
        QCOMPARE(triggered.count(), 1);
        QCOMPARE(triggered.first().at(0).toString(), transfer ? replay : screenshot);
        QCOMPARE(triggered.first().at(1).toInt(), transfer ? 2 : 1);
        QCOMPARE(triggered.first().at(2).toUInt(), oldChord.vk);
        QCOMPARE(triggered.first().at(3).toUInt(), oldChord.modifiers);
        // The working chord is still reserved at the OS, not merely in a map.
        const auto available = [&] {
            return std::async(std::launch::async, [&] {
                if (!RegisterHotKey(nullptr, 1, oldChord.modifiers, oldChord.vk))
                    return false;
                UnregisterHotKey(nullptr, 1);
                return true;
            }).get();
        };
        QVERIFY(!available());
        if (!transfer) {
            releaseReservation();
            hotkeys.synchronize(desired);
            QVERIFY(available()); // the replacement succeeded; F9 is now free
            QVERIFY(fire(hotkeys));
            QCOMPARE(triggered.last().at(2).toUInt(), busyChord.vk);
        }
        hotkeys.synchronize({});
        QVERIFY(available());
    }

    void shortcutErrorsUseTheApplicationLanguage()
    {
        QCOMPARE(HotkeyManager::parseChord("").rejectionReason, QStringLiteral("Choose a shortcut key."));
        QVERIFY(HotkeyManager::parseChord("G").rejectionReason.startsWith(QStringLiteral("Use Ctrl")));
        QVERIFY(HotkeyManager::parseChord("F12").rejectionReason.startsWith(QStringLiteral("Windows reserves")));
        class TestTranslator : public QTranslator {
            bool isEmpty() const override { return false; }
            QString translate(const char*, const char* id, const char*, int) const override {
                const QString prefix = QStringLiteral("gamehq.input.shortcut.");
                const QString key = QString::fromUtf8(id);
                return key.startsWith(prefix) ? key.mid(prefix.size()) : QString();
            }
        } translator;
        QVERIFY(QCoreApplication::installTranslator(&translator));
        QCOMPARE(HotkeyManager::parseChord("").rejectionReason, QStringLiteral("choose_key"));
        QCOMPARE(HotkeyManager::parseChord("G").rejectionReason, QStringLiteral("modifier_required"));
        QCOMPARE(HotkeyManager::parseChord("Ctrl+F12").rejectionReason, QStringLiteral("reserved_f12"));
    }

    void functionKeysAndReservedChords()
    {
        const auto f10 = HotkeyManager::parseChord("F10");
        QVERIFY(f10.valid);
        QCOMPARE(f10.vk, quint32(VK_F10));
        const auto withModifiers = HotkeyManager::parseChord("Ctrl+Shift+F10");
        QVERIFY(withModifiers.valid);
        QCOMPARE(withModifiers.modifiers, quint32(MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT));
        QVERIFY(!HotkeyManager::parseChord("G").valid);
        QVERIFY(!HotkeyManager::parseChord("Win+G").valid);
        for (const QString& reserved : {QStringLiteral("F12"), QStringLiteral("Ctrl+F12")}) {
            const auto rejected = HotkeyManager::parseChord(reserved);
            QVERIFY(!rejected.valid);
            QVERIFY(rejected.rejectionReason.contains("F10"));
        }
    }

    void postedThreadHotkeyFiresOnceAndFollowsItsNewOwner()
    {
        HotkeyManager hotkeys;
        const auto chord = HotkeyManager::parseChord("Ctrl+Shift+Alt+F23");
        QVERIFY(chord.valid);
        const QString action = QStringLiteral("global.screenshot");
        if (!hotkeys.applyBindingSlot(action, 1, chord.modifiers, chord.vk))
            QSKIP("Test chord already in use");
        QSignalSpy triggered(&hotkeys, &HotkeyManager::hotkeyTriggered);
        const auto post = [&] {
            return PostThreadMessageW(GetCurrentThreadId(), WM_HOTKEY, 1,
                                      MAKELPARAM(chord.modifiers & ~MOD_NOREPEAT, chord.vk));
        };
        QVERIFY(post());
        QTRY_COMPARE(triggered.size(), 1);
        QCOMPARE(triggered.takeFirst().at(0).toString(), action);
        hotkeys.synchronize({{QStringLiteral("global.save_replay"), 2, chord}});
        // A distinct press is outside the existing debounce window.
        QTest::qWait(260);
        QVERIFY(post());
        QTRY_COMPARE(triggered.size(), 1);
        const auto event = triggered.takeFirst();
        QCOMPARE(event.at(0).toString(), QStringLiteral("global.save_replay"));
        QCOMPARE(event.at(1).toInt(), 2);
        hotkeys.synchronize({});
        QVERIFY(!fire(hotkeys));
    }

    void slotClearingItsOwnBindingStillDeliversTheAction()
    {
        HotkeyManager hotkeys;
        const QString action = QStringLiteral("global.toggle_overlay");
        if (!hotkeys.applyBinding(action,
                                  MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                                  VK_F24))
            QSKIP("Ctrl+Alt+Shift+F24 is registered by another process");

        int overlayEdges = 0;
        QStringList triggered;
        connect(&hotkeys, &HotkeyManager::overlayTogglePressed, &hotkeys,
                [&hotkeys, &action, &overlayEdges] {
                    ++overlayEdges;
                    hotkeys.clearBinding(action);   // erases the dispatched binding
                });
        connect(&hotkeys, &HotkeyManager::hotkeyTriggered, &hotkeys,
                [&triggered](const QString& id) { triggered.append(id); });

        QVERIFY(fire(hotkeys));
        QCOMPARE(overlayEdges, 1);
        QCOMPARE(triggered, QStringList{ action });

        // The binding really is gone: nothing left to fire.
        QVERIFY(!fire(hotkeys));
    }

    void slotRebindingItsOwnBindingStillDeliversTheAction()
    {
        HotkeyManager hotkeys;
        const QString action = QStringLiteral("global.toggle_overlay");
        if (!hotkeys.applyBinding(action,
                                  MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                                  VK_F23))
            QSKIP("Ctrl+Alt+Shift+F23 is registered by another process");

        QStringList triggered;
        connect(&hotkeys, &HotkeyManager::overlayTogglePressed, &hotkeys,
                [&hotkeys, &action] {
                    // Replace the dispatched binding, then grow the table so a
                    // rehash of m_bindings can relocate its nodes mid-dispatch.
                    hotkeys.applyBinding(action,
                                         MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                                         VK_F22);
                    for (int i = 0; i < 8; ++i)
                        hotkeys.applyBinding(QStringLiteral("test.aux%1").arg(i),
                                             MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                                             VK_F13 + i);
                });
        connect(&hotkeys, &HotkeyManager::hotkeyTriggered, &hotkeys,
                [&triggered](const QString& id) { triggered.append(id); });

        QVERIFY(fire(hotkeys));
        QCOMPARE(triggered, QStringList{ action });
    }
};

QTEST_GUILESS_MAIN(HotkeyReentrancyTest)
#include "tst_hotkeyreentrancy.moc"
