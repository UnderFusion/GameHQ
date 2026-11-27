#include "config/Paths.h"

#include <QDir>
#include <QString>
#include <QTest>

// A portable GameHQ stores paths below its own folder as "portable:/..." so the
// package can be moved. Deciding what is "below" was left to
// QDir::relativeFilePath, which hands an absolute path straight back when the
// target is on another drive or on a UNC share. That result does not start with
// "../", so it was filed as portable and later resolved as
// <package>/D:/Shots/... — the capture was simply gone.
class TestStoredPaths : public QObject
{
    Q_OBJECT

private:
    static QString root() { return QStringLiteral("C:/Games/GameHQ"); }

    static QString stored(const QString& path) { return Paths::toStoredPath(path, root()); }
    static QString resolved(const QString& path) { return Paths::fromStoredPath(path, root()); }

private slots:
    void pathsInsideThePackageStayPortable()
    {
        QCOMPARE(stored(QStringLiteral("C:/Games/GameHQ/Captures/Doom/shot.png")),
                 QStringLiteral("portable:/Captures/Doom/shot.png"));
        // And come back unchanged, which is what makes moving the folder safe.
        QCOMPARE(resolved(QStringLiteral("portable:/Captures/Doom/shot.png")),
                 QStringLiteral("C:/Games/GameHQ/Captures/Doom/shot.png"));
        // A round trip is the property that actually matters.
        QCOMPARE(resolved(stored(QStringLiteral("C:/Games/GameHQ/Captures/Doom/shot.png"))),
                 QStringLiteral("C:/Games/GameHQ/Captures/Doom/shot.png"));
    }

    void aPathOnAnotherDriveStaysAbsolute()
    {
        const QString elsewhere = QStringLiteral("D:/Shots/Doom/shot.png");
        QCOMPARE(stored(elsewhere), elsewhere);
        QCOMPARE(resolved(stored(elsewhere)), elsewhere);
    }

    void aUncPathStaysAbsolute()
    {
        const QString share = QStringLiteral("//nas/media/Doom/shot.png");
        QCOMPARE(stored(share), share);
        QCOMPARE(resolved(stored(share)), share);
    }

    void aPathBesideThePackageStaysAbsolute()
    {
        // Same drive, but outside the package: relativeFilePath would answer
        // "../Elsewhere/shot.png", which must not be stored as portable.
        const QString sibling = QStringLiteral("C:/Games/Elsewhere/shot.png");
        QCOMPARE(stored(sibling), sibling);
        QCOMPARE(resolved(stored(sibling)), sibling);
    }

    void aSiblingSharingTheRootNameStaysAbsolute()
    {
        const QString sibling = QStringLiteral("C:/Games/GameHQ Archive/shot.png");
        QCOMPARE(stored(sibling), sibling);
        QCOMPARE(resolved(stored(sibling)), sibling);
    }

    void aPortablePathWithSpacesRoundTrips()
    {
        const QString local = QStringLiteral("C:/Games/GameHQ/Captures/Game Name/clip 01.mp4");
        QCOMPARE(stored(local), QStringLiteral("portable:/Captures/Game Name/clip 01.mp4"));
        QCOMPARE(resolved(stored(local)), local);
    }

    void rowsWrittenByTheOldBugStillResolve()
    {
        // What a previous build persisted for a capture on another drive. It was
        // never relative to the package, so it must resolve to what it says
        // rather than to <package>/D:/Shots/...
        QCOMPARE(resolved(QStringLiteral("portable:/D:/Shots/Doom/shot.png")),
                 QStringLiteral("D:/Shots/Doom/shot.png"));
        // The old code concatenated the prefix with the untouched UNC path, so
        // the stored value really did carry three slashes.
        QCOMPARE(resolved(QStringLiteral("portable:///nas/media/Doom/shot.png")),
                 QStringLiteral("//nas/media/Doom/shot.png"));
        // Re-storing such a row cleans it up instead of preserving the mistake.
        QCOMPARE(stored(QStringLiteral("portable:/D:/Shots/Doom/shot.png")),
                 QStringLiteral("D:/Shots/Doom/shot.png"));
    }

    void theStoredPrefixIsRecognisedWhateverItsCase()
    {
        QCOMPARE(resolved(QStringLiteral("Portable:/Captures/Doom/shot.png")),
                 QStringLiteral("C:/Games/GameHQ/Captures/Doom/shot.png"));
        QCOMPARE(resolved(QStringLiteral("PORTABLE:/Captures/Doom/shot.png")),
                 QStringLiteral("C:/Games/GameHQ/Captures/Doom/shot.png"));
    }

    void nativeSeparatorsAndWhitespaceAreAccepted()
    {
        QCOMPARE(stored(QStringLiteral("  C:\\Games\\GameHQ\\Captures\\Doom\\shot.png  ")),
                 QStringLiteral("portable:/Captures/Doom/shot.png"));
        QCOMPARE(stored(QStringLiteral("D:\\Shots\\Doom\\shot.png")),
                 QStringLiteral("D:/Shots/Doom/shot.png"));
    }

    void anInstalledCopyStoresEverythingAbsolutely()
    {
        // No portable root: nothing is ever rewritten to a portable form.
        QCOMPARE(Paths::toStoredPath(QStringLiteral("C:/Games/GameHQ/Captures/Doom/shot.png"),
                                     QString()),
                 QStringLiteral("C:/Games/GameHQ/Captures/Doom/shot.png"));
        QCOMPARE(Paths::toStoredPath(QStringLiteral("D:/Shots/Doom/shot.png"), QString()),
                 QStringLiteral("D:/Shots/Doom/shot.png"));
    }

    void emptyInputStaysEmpty()
    {
        QVERIFY(stored(QString()).isEmpty());
        QVERIFY(stored(QStringLiteral("   ")).isEmpty());
        QVERIFY(resolved(QString()).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestStoredPaths)
#include "tst_storedpaths.moc"
