// icongen — renders assets/icons/gamehq.svg to the PNG sizes needed for gamehq.ico.
// One-off asset tool, not part of the app build. Recipe (Git Bash, repo root;
// the exe must sit in build/app/ so the deployed Qt DLLs + platform plugin resolve,
// and the sandbox requires absolute in-project paths — no ".."):
//   QT=tools/Qt/6.8.3/mingw_64
//   tools/Qt/Tools/mingw1310_64/bin/g++.exe assets/icons/icongen.cpp \
//     -I$QT/include -I$QT/include/QtCore -I$QT/include/QtGui -I$QT/include/QtSvg \
//     -L$QT/lib -lQt6Svg -lQt6Gui -lQt6Core -o build/app/icongen_tmp.exe
//   (cd build/app && ./icongen_tmp.exe ../../assets/icons/gamehq.svg \
//     ../../out/tmp/icon-png)
//   python assets/icons/generate_icon.py out/tmp/icon-png assets/icons/gamehq.ico
//   rm build/app/icongen_tmp.exe
#include <QGuiApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <cstdio>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: icongen <svg> <outdir>\n");
        return 1;
    }
    QSvgRenderer svg(QString::fromLocal8Bit(argv[1]));
    if (!svg.isValid()) {
        std::fprintf(stderr, "icongen: cannot load %s\n", argv[1]);
        return 1;
    }
    const QString outDir = QString::fromLocal8Bit(argv[2]);
    QDir().mkpath(outDir);
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    for (int s : sizes) {
        QImage img(s, s, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        svg.render(&p);
        p.end();
        const QString path = QStringLiteral("%1/icon_%2.png").arg(outDir).arg(s);
        if (!img.save(path)) {
            std::fprintf(stderr, "icongen: cannot save %s\n", qPrintable(path));
            return 1;
        }
        std::printf("icongen: wrote %s\n", qPrintable(path));
    }
    return 0;
}
