#include "telegram/TdRuntime.h"

#include "telegram/TdRuntimePin.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace telegram
{

TdRuntime::TdRuntime(QString dllPath, QString expectedSha256, QString expectedVersion)
    : m_dllPath(std::move(dllPath))
    , m_expectedSha256(expectedSha256.trimmed().toLower())
    , m_expectedVersion(std::move(expectedVersion))
{
}

TdRuntime::~TdRuntime() = default;

std::unique_ptr<TdRuntime> TdRuntime::forInstalledApp(const QString& applicationDir)
{
    const QString path = QDir(applicationDir).filePath(
        QString(tdpin::kRuntimeRelativeDir) + QLatin1Char('/') + QString(tdpin::kRuntimeFileName));
    return std::make_unique<TdRuntime>(path, QString(tdpin::kRuntimeSha256),
                                       QString(tdpin::kVersion));
}

QString TdRuntime::statusCode(Status status)
{
    switch (status) {
    case Status::NotLoaded:    return QStringLiteral("not_loaded");
    case Status::Ready:        return QStringLiteral("ready");
    case Status::Unpinned:     return QStringLiteral("unpinned");
    case Status::NotInstalled: return QStringLiteral("not_installed");
    case Status::HashMismatch: return QStringLiteral("hash_mismatch");
    case Status::LoadFailed:   return QStringLiteral("load_failed");
    case Status::Incompatible: return QStringLiteral("incompatible");
    }
    return QStringLiteral("load_failed");
}

QString TdRuntime::sha256OfFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

TdRuntime::Status TdRuntime::load()
{
    if (m_status != Status::NotLoaded)
        return m_status;

    const auto finish = [this](Status s) {
        m_status = s;
        qInfo() << "Telegram: TDLib runtime" << statusCode(s);
        return s;
    };

    if (m_expectedSha256.isEmpty())
        return finish(Status::Unpinned);
    if (!QFileInfo(m_dllPath).isFile())
        return finish(Status::NotInstalled);
    // Hash before loading: a DLL that is not the pinned build never runs code.
    if (sha256OfFile(m_dllPath) != m_expectedSha256)
        return finish(Status::HashMismatch);

    auto lib = std::make_unique<QLibrary>(QFileInfo(m_dllPath).absoluteFilePath());
    if (!lib->load())
        return finish(Status::LoadFailed);

    TdJsonApi api;
    api.createClientId = reinterpret_cast<TdJsonApi::CreateClientId>(lib->resolve("td_create_client_id"));
    api.send = reinterpret_cast<TdJsonApi::Send>(lib->resolve("td_send"));
    api.receive = reinterpret_cast<TdJsonApi::Receive>(lib->resolve("td_receive"));
    api.execute = reinterpret_cast<TdJsonApi::Execute>(lib->resolve("td_execute"));
    api.setLogMessageCallback = reinterpret_cast<TdJsonApi::SetLogCallback>(
        lib->resolve("td_set_log_message_callback"));
    if (!api.isValid()) {
        lib->unload();
        return finish(Status::Incompatible);
    }

    // Quiet TDLib before anything else: its log can name chats and files.
    api.execute("{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":0}");

    const char* reply = api.execute("{\"@type\":\"getOption\",\"name\":\"version\"}");
    QString version;
    if (reply) {
        const QJsonObject o = QJsonDocument::fromJson(QByteArray(reply)).object();
        version = o.value(QLatin1String("value")).toString();
    }
    if (version.isEmpty() || version != m_expectedVersion) {
        lib->unload();
        return finish(Status::Incompatible);
    }

    m_reportedVersion = version;
    m_api = api;
    m_library = std::move(lib);
    return finish(Status::Ready);
}

} // namespace telegram
