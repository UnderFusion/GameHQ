#include "diagnostics/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

namespace
{
QFile g_logFile;
QMutex g_mutex;
QtMessageHandler g_previous = nullptr;
QString g_logDirectory;
bool g_handlerInstalled = false;
// Lines logged between beginBuffering() and install(). Bounded so a process
// that never reaches install() cannot grow without limit.
QStringList g_pending;
constexpr qsizetype kMaxPendingLines = 1000;
// Tracked rather than stat'ed: every line is flushed already, and asking the
// filesystem for the size on each one would double that cost.
qint64 g_bytesInFile = 0;

const QString& logFileName()
{
    static const QString name = QStringLiteral("gamehq.log");
    return name;
}

const char* levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "DBG";
    case QtInfoMsg:     return "INF";
    case QtWarningMsg:  return "WRN";
    case QtCriticalMsg: return "ERR";
    case QtFatalMsg:    return "FTL";
    }
    return "???";
}

// Caller holds g_mutex.
bool openLogFile()
{
    g_logFile.setFileName(g_logDirectory + QLatin1Char('/') + logFileName());
    if (!g_logFile.open(QIODevice::Append | QIODevice::Text)) {
        g_bytesInFile = 0;
        return false;
    }
    g_bytesInFile = g_logFile.size();
    return true;
}

// Caller holds g_mutex.
void writeLine(const QString& line)
{
    QTextStream out(&g_logFile);
    out << line << '\n';
    out.flush();
    g_bytesInFile += line.size() + 1;
}

// Caller holds g_mutex.
void writeLineToStderr(const QString& line)
{
    std::fputs(qPrintable(line), stderr);
    std::fputc('\n', stderr);
}

// Caller holds g_mutex.
void rotateIfFull()
{
    if (g_bytesInFile < Logger::kMaxLogBytes)
        return;
    g_logFile.close();
    // If rotation could not move the files, reopening simply continues the
    // existing log: growing is better than dropping messages on the floor.
    Logger::rotateIfNeeded(g_logDirectory, logFileName(), Logger::kMaxLogBytes,
                           Logger::kRetainedLogs);
    openLogFile();
}

void handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    const QString line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
        + QLatin1Char(' ') + QLatin1String(levelName(type)) + QLatin1Char(' ') + msg;
    {
        QMutexLocker lock(&g_mutex);
        if (g_logFile.isOpen()) {
            rotateIfFull();
            if (g_logFile.isOpen())
                writeLine(line);
        } else if (!g_logDirectory.isEmpty()) {
            // The log could not be opened. Say it somewhere rather than run a
            // whole session silently.
            writeLineToStderr(line);
        } else if (g_pending.size() < kMaxPendingLines) {
            g_pending.append(line);
        }
    }
#ifdef QT_DEBUG
    if (g_previous)
        g_previous(type, ctx, msg);
#else
    Q_UNUSED(ctx);
#endif
}
} // namespace

namespace Logger
{

void beginBuffering()
{
    {
        QMutexLocker lock(&g_mutex);
        if (g_handlerInstalled)
            return;
        g_handlerInstalled = true;
    }
    g_previous = qInstallMessageHandler(handler);
}

void install(const QString& logsDir)
{
    bool alreadyInstalled = false;
    bool opened = false;
    {
        QMutexLocker lock(&g_mutex);
        if (g_logFile.isOpen() && g_logDirectory == logsDir)
            return;
        alreadyInstalled = g_handlerInstalled;
        g_handlerInstalled = true;
        if (g_logFile.isOpen())
            g_logFile.close();
        g_logDirectory = logsDir;
        QDir().mkpath(logsDir);
        opened = openLogFile();
        // Buffered lines keep their original timestamps and go first, in
        // order, so the file reads as if it had been open all along.
        for (const QString& line : std::as_const(g_pending)) {
            if (opened) {
                rotateIfFull();
                if (g_logFile.isOpen())
                    writeLine(line);
            } else {
                writeLineToStderr(line);
            }
        }
        g_pending.clear();
    }
    if (!opened) {
        std::fputs("GameHQ: the log file could not be opened; "
                   "diagnostics go to stderr for this session\n", stderr);
    }
    // Installing the handler a second time would hand back this very function
    // as the "previous" one, and a debug build chains to it — straight into
    // unbounded recursion.
    if (!alreadyInstalled)
        g_previous = qInstallMessageHandler(handler);
}

QString filePath(const QString& logsDir)
{
    return logsDir + QLatin1Char('/') + logFileName();
}

bool writingToFile()
{
    QMutexLocker lock(&g_mutex);
    return g_logFile.isOpen();
}

bool rotateIfNeeded(const QString& directory, const QString& baseName,
                    qint64 maxBytes, int retained)
{
    const QString current = directory + QLatin1Char('/') + baseName;
    const QFileInfo info(current);
    if (!info.exists() || info.size() < maxBytes || retained < 1)
        return false;

    const QFileInfo nameParts(baseName);
    const QString stem = nameParts.completeBaseName();
    const QString suffix = nameParts.suffix();
    const auto generation = [&](int index) {
        QString name = directory + QLatin1Char('/') + stem + QLatin1Char('.')
            + QString::number(index);
        if (!suffix.isEmpty())
            name += QLatin1Char('.') + suffix;
        return name;
    };

    // The oldest generation is the one we are allowed to lose.
    if (QFile::exists(generation(retained)) && !QFile::remove(generation(retained)))
        return false;
    for (int index = retained - 1; index >= 1; --index) {
        if (!QFile::exists(generation(index)))
            continue;
        if (!QFile::rename(generation(index), generation(index + 1)))
            return false;
    }
    return QFile::rename(current, generation(1));
}

} // namespace Logger
