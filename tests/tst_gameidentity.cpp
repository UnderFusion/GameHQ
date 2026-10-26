#include "core/GameIdentity.h"

#include <QTest>

// GameIdentity turns a game's display name into a filesystem-safe folder name
// and a comparison key, and reads a game back out of a capture's path. All of
// it is pure string/path arithmetic, so it is testable as-is.
class TestGameIdentity : public QObject
{
    Q_OBJECT

private slots:
    void folderName_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");

        QTest::newRow("plain") << "Hades" << "Hades";
        // Every character Windows forbids in a path segment becomes '_'.
        QTest::newRow("colon") << "Marvel's Spider-Man: Miles Morales"
                               << "Marvel's Spider-Man_ Miles Morales";
        QTest::newRow("slash") << "Ratchet/Clank" << "Ratchet_Clank";
        QTest::newRow("backslash") << "A\\B" << "A_B";
        QTest::newRow("all forbidden") << "<>:\"/\\|?*" << "_________";
        QTest::newRow("trimmed") << "  Hades  " << "Hades";
        // An empty or whitespace-only name would produce an unusable folder.
        QTest::newRow("empty") << "" << "Unknown Game";
        QTest::newRow("whitespace only") << "   " << "Unknown Game";
    }

    void folderName()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(GameIdentity::folderName(input), expected);
    }

    void pathComponent_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");

        // Ordinary names keep the folder they always had.
        QTest::newRow("plain") << "Hades" << "Hades";
        QTest::newRow("colon") << "Spider-Man: Miles" << "Spider-Man_ Miles";
        QTest::newRow("inner dots") << "S.T.A.L.K.E.R. 2" << "S.T.A.L.K.E.R. 2";
        // A dot-only name would climb out of (or be) the capture root.
        QTest::newRow("dot dot") << ".." << "Unknown Game";
        QTest::newRow("dot") << "." << "Unknown Game";
        QTest::newRow("dots and spaces") << ". . ." << "Unknown Game";
        QTest::newRow("trailing dot") << "Game." << "Game";
        QTest::newRow("trailing space dot") << "Game . " << "Game";
        QTest::newRow("control chars") << QStringLiteral("A\tB\x01") << "A_B_";
        // Reserved device names, bare or with an extension, any case.
        QTest::newRow("CON") << "CON" << "CON_";
        QTest::newRow("nul lower") << "nul" << "nul_";
        QTest::newRow("COM1") << "COM1" << "COM1_";
        QTest::newRow("LPT9 ext") << "LPT9.txt" << "LPT9_.txt";
        QTest::newRow("COM0 is fine") << "COM0" << "COM0";
        QTest::newRow("CONTROL is fine") << "Control" << "Control";
    }

    void pathComponent()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(GameIdentity::pathComponent(input), expected);
    }

    void pathComponentNeverChangesTheStoredKey()
    {
        // key() is stored in the database; the path guard must not move it.
        QCOMPARE(GameIdentity::key(QStringLiteral("Hades")), QStringLiteral("hades"));
        QCOMPARE(GameIdentity::key(QStringLiteral("CON")), QStringLiteral("con"));
    }

    void isInsideRoot()
    {
        const QString root = QStringLiteral("D:/Captures");
        QVERIFY(GameIdentity::isInsideRoot(root, root + QStringLiteral("/Hades/Clips")));
        QVERIFY(GameIdentity::isInsideRoot(root + QLatin1Char('/'), root + QStringLiteral("/a")));
        QVERIFY(GameIdentity::isInsideRoot(QStringLiteral("d:\\captures"),
                                           QStringLiteral("D:/Captures/x")));
        QVERIFY(!GameIdentity::isInsideRoot(root, root));
        QVERIFY(!GameIdentity::isInsideRoot(root, root + QStringLiteral("/../Clips")));
        QVERIFY(!GameIdentity::isInsideRoot(root, root + QStringLiteral("/a/../../x")));
        QVERIFY(!GameIdentity::isInsideRoot(root, QStringLiteral("D:/CapturesOther/x")));
        QVERIFY(!GameIdentity::isInsideRoot(QString(), QStringLiteral("D:/x")));
        // Every component pathComponent() can produce stays inside.
        for (const QString& name : { QStringLiteral(".."), QStringLiteral("."),
                                     QStringLiteral("../.."), QStringLiteral("CON") }) {
            QVERIFY2(GameIdentity::isInsideRoot(
                         root, root + QLatin1Char('/') + GameIdentity::pathComponent(name)
                                   + QStringLiteral("/Clips")),
                     qPrintable(name));
        }
    }

    void key_foldsCaseAndWhitespace()
    {
        // The key exists so two spellings of one game collapse to one row.
        QCOMPARE(GameIdentity::key("HADES"), GameIdentity::key("hades"));
        QCOMPARE(GameIdentity::key("Hades   II"), GameIdentity::key("Hades II"));
        QCOMPARE(GameIdentity::key("  Hades  "), GameIdentity::key("Hades"));
    }

    void key_appliesFolderSanitizingFirst()
    {
        // A raw title and its sanitized folder name must land on the same key,
        // otherwise a rescan would duplicate the game row.
        QCOMPARE(GameIdentity::key("Ratchet/Clank"), GameIdentity::key("Ratchet_Clank"));
    }

    void key_keepsDistinctGamesDistinct()
    {
        QVERIFY(GameIdentity::key("Hades") != GameIdentity::key("Hades II"));
    }

    void hasFolderForbiddenChar_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<bool>("expected");

        QTest::newRow("clean") << "Hades" << false;
        QTest::newRow("empty") << "" << false;
        QTest::newRow("colon") << "Halo: Reach" << true;
        QTest::newRow("question") << "Who?" << true;
        QTest::newRow("apostrophe is allowed") << "Marvel's" << false;
    }

    void hasFolderForbiddenChar()
    {
        QFETCH(QString, input);
        QFETCH(bool, expected);
        QCOMPARE(GameIdentity::hasFolderForbiddenChar(input), expected);
    }

    void inferFromPath_data()
    {
        QTest::addColumn<QString>("root");
        QTest::addColumn<QString>("filePath");
        QTest::addColumn<QString>("expected");

        QTest::newRow("screenshots")
            << "C:/Captures" << "C:/Captures/Hades/Screenshots/shot.png" << "Hades";
        QTest::newRow("clips")
            << "C:/Captures" << "C:/Captures/Hades/Clips/clip.mp4" << "Hades";
        QTest::newRow("directly in game folder")
            << "C:/Captures" << "C:/Captures/Hades/shot.png" << "Hades";
        QTest::newRow("deeper nesting still reads the game folder")
            << "C:/Captures" << "C:/Captures/Hades/Screenshots/2026/shot.png" << "Hades";
        // No game folder between root and file → nothing to infer.
        QTest::newRow("loose file in root")
            << "C:/Captures" << "C:/Captures/shot.png" << "Unknown Game";
        QTest::newRow("trailing slash on root")
            << "C:/Captures/" << "C:/Captures/Hades/Screenshots/shot.png" << "Hades";
        QTest::newRow("game name with spaces")
            << "C:/Captures" << "C:/Captures/Hades II/Clips/clip.mp4" << "Hades II";
    }

    void inferFromPath()
    {
        QFETCH(QString, root);
        QFETCH(QString, filePath);
        QFETCH(QString, expected);
        QCOMPARE(GameIdentity::inferFromPath(root, filePath), expected);
    }
};

QTEST_APPLESS_MAIN(TestGameIdentity)
#include "tst_gameidentity.moc"
