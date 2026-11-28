#include "storage/GameIconCache.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

// GameIconCache resolves MSIX/GDK package artwork from a manifest before
// falling back to the shell icon. These tests exercise the manifest-parsing
// and asset-selection helpers directly (real temp files, no Shell API, no
// writes to the real gamehq-data cache) plus the early-exit paths of
// iconPathForExecutable that never touch the filesystem cache at all.
class TestGameIconCache : public QObject
{
    Q_OBJECT

private slots:
    void emptyExecutablePathIsRejected()
    {
        QVERIFY(GameIconCache::iconPathForExecutable(QString()).isEmpty());
    }

    void nonexistentExecutablePathIsRejected()
    {
        QVERIFY(GameIconCache::iconPathForExecutable(
                    QStringLiteral("C:/definitely/not/a/real/path.exe")).isEmpty());
    }

    void logoReferencesAreRankedLargestTileFirst()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifestPath = dir.filePath(QStringLiteral("AppxManifest.xml"));
        QFile file(manifestPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        // Attributes deliberately listed out of priority order, to prove the
        // function ranks them rather than returning first-seen-first.
        //
        // The xmlns:uap URI is built at runtime from two QStringLiteral tokens
        // ("http:" + "//" + "schemas...") on purpose. The Qt moc lexer treats
        // `//` inside any single string literal (raw or ordinary, even when
        // split across adjacent literals that the C++ compiler would later
        // concatenate) as a comment and then fails to find the Q_OBJECT class,
        // producing an empty .moc file. Building the URI from `+` at runtime
        // keeps the real Microsoft namespace URI in the test while making moc
        // see only one `/` per token.
        const QString nsUri = QStringLiteral("http:") + QStringLiteral("//") +
                              QStringLiteral("schemas.microsoft.com/appx/manifest/uap/windows10");
        file.write(QStringLiteral(
            "<?xml version=\"1.0\"?>"
            "<Package xmlns:uap=\"%1\">"
            "<Applications><Application>"
            "<uap:VisualElements Square44x44Logo=\"Assets\\Square44x44Logo.png\" "
            "Square150x150Logo=\"Assets\\Square150x150Logo.png\" "
            "Square480x480Logo=\"Assets\\Square480x480Logo.png\" />"
            "</Application></Applications>"
            "</Package>").arg(nsUri).toUtf8());
        file.close();

        const QStringList refs = GameIconCache::manifestLogoReferencesForTesting(manifestPath);
        QCOMPARE(refs.size(), 3);
        QCOMPARE(refs.at(0), QStringLiteral("Assets\\Square480x480Logo.png"));
        QCOMPARE(refs.at(1), QStringLiteral("Assets\\Square150x150Logo.png"));
        QCOMPARE(refs.at(2), QStringLiteral("Assets\\Square44x44Logo.png"));
    }

    void malformedManifestYieldsNoReferencesSafely()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifestPath = dir.filePath(QStringLiteral("AppxManifest.xml"));
        QFile file(manifestPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        // "uap:" prefix used without a xmlns:uap declaration: not well-formed
        // XML. The parser must fail safely and return no references, not crash.
        file.write(R"(<?xml version="1.0"?>
<Package>
  <Applications>
    <Application>
      <uap:VisualElements Square44x44Logo="Assets\Square44x44Logo.png" />
    </Application>
  </Applications>
</Package>)");
        file.close();

        QVERIFY(GameIconCache::manifestLogoReferencesForTesting(manifestPath).isEmpty());
    }

    void logoElementFormIsAlsoRecognized()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifestPath = dir.filePath(QStringLiteral("AppxManifest.xml"));
        QFile file(manifestPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"(<?xml version="1.0"?>
<Package>
  <Properties>
    <Logo>Assets\StoreLogo.png</Logo>
  </Properties>
</Package>)");
        file.close();

        const QStringList refs = GameIconCache::manifestLogoReferencesForTesting(manifestPath);
        QCOMPARE(refs, QStringList{ QStringLiteral("Assets\\StoreLogo.png") });
    }

    void manifestWithNoRecognizedLogoYieldsNoReferences()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString manifestPath = dir.filePath(QStringLiteral("AppxManifest.xml"));
        QFile file(manifestPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"(<?xml version="1.0"?><Package><Identity Name="Foo" /></Package>)");
        file.close();

        QVERIFY(GameIconCache::manifestLogoReferencesForTesting(manifestPath).isEmpty());
    }

    void missingManifestFileYieldsNoReferences()
    {
        QVERIFY(GameIconCache::manifestLogoReferencesForTesting(
                    QStringLiteral("C:/no/such/manifest.xml")).isEmpty());
    }

    void assetResolutionPrefersHighestResolutionQualifiedVariant()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Assets")));

        auto writeBytes = [&](const QString& relative, int size) {
            QFile f(dir.filePath(relative));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(size, 'x'));
        };
        // Three qualified variants of the same logical asset; the largest
        // file (highest resolution) must be the one picked.
        writeBytes(QStringLiteral("Assets/Square150x150Logo.scale-100.png"), 100);
        writeBytes(QStringLiteral("Assets/Square150x150Logo.scale-150.png"), 300);
        writeBytes(QStringLiteral("Assets/Square150x150Logo.scale-200.png"), 500);

        const QString resolved = GameIconCache::resolveManifestAssetForTesting(
            dir.path(), QStringLiteral("Assets\\Square150x150Logo.png"));
        QCOMPARE(resolved, QDir(dir.filePath(QStringLiteral("Assets")))
                                .filePath(QStringLiteral("Square150x150Logo.scale-200.png")));
    }

    void assetResolutionReturnsDirectFileWhenPresent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Assets")));
        QFile f(dir.filePath(QStringLiteral("Assets/Logo.png")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("data");
        f.close();

        const QString resolved = GameIconCache::resolveManifestAssetForTesting(
            dir.path(), QStringLiteral("Assets\\Logo.png"));
        QCOMPARE(resolved, dir.filePath(QStringLiteral("Assets/Logo.png")));
    }

    void assetResolutionReturnsEmptyWhenNothingOnDisk()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString resolved = GameIconCache::resolveManifestAssetForTesting(
            dir.path(), QStringLiteral("Assets\\Missing.png"));
        QVERIFY(resolved.isEmpty());
    }

    void paddedJumboTileIsCroppedToTheIconFrame()
    {
        // #42: a 48 px icon painted into the corner of a transparent 256 px tile.
        QImage tile(256, 256, QImage::Format_ARGB32);
        tile.fill(Qt::transparent);
        for (int y = 2; y < 46; ++y)
            for (int x = 3; x < 45; ++x)
                tile.setPixel(x, y, qRgba(200, 40, 40, 255));
        const QImage cropped = GameIconCache::cropShellIconPaddingForTesting(tile);
        QCOMPARE(cropped.size(), QSize(48, 48));
        QCOMPARE(qAlpha(cropped.pixel(3, 2)), 255);
        QCOMPARE(qAlpha(cropped.pixel(0, 0)), 0);
    }

    void fullSizeAndCentredIconsAreLeftAlone()
    {
        QImage full(256, 256, QImage::Format_ARGB32);
        full.fill(Qt::transparent);
        for (int y = 20; y < 236; ++y)
            for (int x = 20; x < 236; ++x)
                full.setPixel(x, y, qRgba(10, 120, 200, 255));
        QCOMPARE(GameIconCache::cropShellIconPaddingForTesting(full).size(), QSize(256, 256));

        // Deliberately small artwork centred in the tile is the author's choice.
        QImage centred(256, 256, QImage::Format_ARGB32);
        centred.fill(Qt::transparent);
        for (int y = 100; y < 156; ++y)
            for (int x = 100; x < 156; ++x)
                centred.setPixel(x, y, qRgba(10, 200, 10, 255));
        QCOMPARE(GameIconCache::cropShellIconPaddingForTesting(centred).size(), QSize(256, 256));

        // A badge inside the top-left quadrant but away from the origin.
        QImage badge(256, 256, QImage::Format_ARGB32);
        badge.fill(Qt::transparent);
        for (int y = 64; y < 128; ++y)
            for (int x = 64; x < 128; ++x)
                badge.setPixel(x, y, qRgba(200, 200, 10, 255));
        QCOMPARE(GameIconCache::cropShellIconPaddingForTesting(badge).size(), QSize(256, 256));

        QImage empty(256, 256, QImage::Format_ARGB32);
        empty.fill(Qt::transparent);
        QCOMPARE(GameIconCache::cropShellIconPaddingForTesting(empty).size(), QSize(256, 256));
    }

    void formatVersionIsNonEmptyAndStable()
    {
        // Cache-key input for CaptureDatabase's re-extraction sentinel; it must
        // never be empty, and calling it twice must be stable within a run.
        const QString version = GameIconCache::formatVersion();
        QVERIFY(!version.isEmpty());
        QCOMPARE(version, GameIconCache::formatVersion());
    }
};

QTEST_APPLESS_MAIN(TestGameIconCache)
#include "tst_gameiconcache.moc"
