#include "share/providers/DiscordWebhookUpload.h"

#include <QNetworkProxy>
#include <QSslSocket>
#include <QUuid>

namespace share
{

namespace
{
constexpr qint64 kChunkBytes = 64 * 1024;
constexpr qint64 kMaxQueuedBytes = 256 * 1024;   // socket backlog we allow
constexpr qsizetype kMaxResponseBytes = 256 * 1024;

QByteArray headerSafe(const QString& text)
{
    QString out = text;
    out.remove(QLatin1Char('"'));
    out.remove(QLatin1Char('\\'));
    out.remove(QLatin1Char('\r'));
    out.remove(QLatin1Char('\n'));
    return out.toUtf8();
}

// Decodes an HTTP/1.1 chunked body; `complete` says the terminating chunk was
// seen. Malformed input yields what was decoded so far.
QByteArray decodeChunked(const QByteArray& raw, bool* complete)
{
    QByteArray out;
    *complete = false;
    qsizetype pos = 0;
    while (pos < raw.size()) {
        const qsizetype eol = raw.indexOf("\r\n", pos);
        if (eol < 0)
            break;
        bool ok = false;
        const qint64 size = raw.mid(pos, eol - pos).split(';').first().trimmed().toLongLong(&ok, 16);
        if (!ok || size < 0)
            break;
        if (size == 0) {
            *complete = true;
            break;
        }
        pos = eol + 2;
        if (raw.size() < pos + size)
            break;
        out.append(raw.mid(pos, size));
        pos += size + 2;
    }
    return out;
}
} // namespace

DiscordWebhookUpload::DiscordWebhookUpload(QObject* parent)
    : QObject(parent)
    , m_socket(new QSslSocket(this))
{
    // A webhook is direct: never route a credential through an ambient proxy
    // we did not ask for, and never trust anything but the system roots.
    m_socket->setProxy(QNetworkProxy::NoProxy);
    connect(m_socket, &QSslSocket::encrypted, this, [this] { begin(); });
    connect(m_socket, &QSslSocket::connected, this, [this] {
        if (!m_secure)
            begin();
    });
    connect(m_socket, &QSslSocket::bytesWritten, this, [this](qint64 n) {
        if (!m_started || m_done)
            return;
        m_written += n;
        emit progress(m_written, m_total);
        pump();
    });
    connect(m_socket, &QSslSocket::readyRead, this, [this] { onReadyRead(); });
    connect(m_socket, &QSslSocket::disconnected, this, [this] { onDisconnected(); });
    connect(m_socket, &QSslSocket::errorOccurred, this, [this] { onSocketError(); });
    // sslErrors is deliberately NOT ignored: a bad certificate ends the
    // handshake with SslHandshakeFailedError.
}

DiscordWebhookUpload::~DiscordWebhookUpload() = default;

void DiscordWebhookUpload::start(const QUrl& endpoint, const QByteArray& payloadJson, const Part& file)
{
    m_url = endpoint;
    m_secure = endpoint.scheme() == QLatin1String("https");
    m_file.setFileName(file.path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        complete(0, {}, Failure::Io);
        return;
    }
    m_fileLeft = m_file.size();

    const QByteArray boundary = "----GameHQ" + QUuid::createUuid().toString(QUuid::Id128).toUtf8();
    QByteArray preamble;
    preamble += "--" + boundary + "\r\n";
    preamble += "Content-Disposition: form-data; name=\"payload_json\"\r\n";
    preamble += "Content-Type: application/json\r\n\r\n";
    preamble += payloadJson + "\r\n";
    preamble += "--" + boundary + "\r\n";
    preamble += "Content-Disposition: form-data; name=\"files[0]\"; filename=\""
                + headerSafe(file.fileName) + "\"\r\n";
    preamble += "Content-Type: " + file.mimeType.toUtf8() + "\r\n\r\n";
    m_tail = "\r\n--" + boundary + "--\r\n";
    const qint64 bodyBytes = preamble.size() + m_fileLeft + m_tail.size();

    const QString target = endpoint.path(QUrl::FullyEncoded)
                           + (endpoint.hasQuery() ? QLatin1Char('?') + endpoint.query(QUrl::FullyEncoded)
                                                  : QString());
    QByteArray hostHeader = endpoint.host().toUtf8();
    if (endpoint.port() > 0)
        hostHeader += ":" + QByteArray::number(endpoint.port());
    QByteArray head;
    head += "POST " + target.toUtf8() + " HTTP/1.1\r\n";
    head += "Host: " + hostHeader + "\r\n";
    head += "User-Agent: GameHQ\r\n";
    head += "Accept: application/json\r\n";
    head += "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
    head += "Content-Length: " + QByteArray::number(bodyBytes) + "\r\n";
    head += "Connection: close\r\n\r\n";
    m_head = head + preamble;
    m_total = m_head.size() + m_fileLeft + m_tail.size();

    const quint16 port = endpoint.port(m_secure ? 443 : 80);
    if (m_secure)
        m_socket->connectToHostEncrypted(endpoint.host(), port);
    else
        m_socket->connectToHost(endpoint.host(), port);
}

void DiscordWebhookUpload::begin()
{
    if (m_started || m_done)
        return;
    m_started = true;
    m_socket->write(m_head);
    m_head.clear();
    pump();
}

void DiscordWebhookUpload::pump()
{
    while (!m_done && !m_queuedAll && m_socket->bytesToWrite() < kMaxQueuedBytes) {
        if (m_fileLeft > 0) {
            const QByteArray chunk = m_file.read(qMin(kChunkBytes, m_fileLeft));
            if (chunk.isEmpty()) {
                // The capture shrank or became unreadable mid-upload: what was
                // sent is an incomplete body and cannot become a message.
                complete(0, {}, Failure::Io);
                return;
            }
            m_fileLeft -= chunk.size();
            m_socket->write(chunk);
        } else {
            m_socket->write(m_tail);
            m_queuedAll = true;
        }
    }
    if (m_queuedAll && m_socket->bytesToWrite() == 0 && !m_fullySent) {
        m_fullySent = true;
        emit progress(m_total, m_total);
    }
}

bool DiscordWebhookUpload::parseResponse(bool connectionClosed)
{
    if (!m_headerParsed) {
        const qsizetype headEnd = m_response.indexOf("\r\n\r\n");
        if (headEnd < 0)
            return false;
        const QList<QByteArray> lines = m_response.left(headEnd).split('\n');
        const QList<QByteArray> status = lines.first().trimmed().split(' ');
        if (status.size() < 2 || !status.first().startsWith("HTTP/"))
            return false;
        m_status = status.at(1).toInt();
        for (const QByteArray& raw : lines.mid(1)) {
            const QByteArray line = raw.trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon < 0)
                continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (name == "content-length")
                m_contentLength = value.toLongLong();
            else if (name == "transfer-encoding" && value.toLower().contains("chunked"))
                m_chunked = true;
        }
        m_bodyStart = headEnd + 4;
        m_headerParsed = m_status >= 100;
        if (!m_headerParsed)
            return false;
    }
    const QByteArray raw = m_response.mid(m_bodyStart);
    if (m_chunked) {
        bool done = false;
        const QByteArray body = decodeChunked(raw, &done);
        if (done || connectionClosed) {
            complete(m_status, body, Failure::None);
            return true;
        }
        return false;
    }
    if (m_contentLength >= 0) {
        if (raw.size() >= m_contentLength || connectionClosed) {
            complete(m_status, raw.left(m_contentLength), Failure::None);
            return true;
        }
        return false;
    }
    if (connectionClosed || m_status == 204 || m_status == 304) {
        complete(m_status, raw, Failure::None);
        return true;
    }
    return false;
}

void DiscordWebhookUpload::onReadyRead()
{
    if (m_done)
        return;
    m_response.append(m_socket->readAll());
    if (m_response.size() > kMaxResponseBytes) {
        // Not a Discord answer; keep what we have as the status only.
        parseResponse(true);
        if (!m_done)
            complete(0, {}, Failure::Io);
        return;
    }
    parseResponse(false);
}

void DiscordWebhookUpload::onDisconnected()
{
    if (m_done)
        return;
    m_response.append(m_socket->readAll());
    if (!parseResponse(true))
        complete(0, {}, Failure::Io);   // closed without a usable answer
}

void DiscordWebhookUpload::onSocketError()
{
    if (m_done)
        return;
    switch (m_socket->error()) {
    case QAbstractSocket::SslHandshakeFailedError:
    case QAbstractSocket::SslInvalidUserDataError:
        complete(0, {}, Failure::Tls);
        break;
    case QAbstractSocket::RemoteHostClosedError:
        break;   // onDisconnected() judges what we got
    default:
        complete(0, {}, m_started ? Failure::Io : Failure::Connect);
        break;
    }
}

void DiscordWebhookUpload::abort()
{
    if (m_done)
        return;
    complete(0, {}, Failure::Aborted);
}

void DiscordWebhookUpload::complete(int status, const QByteArray& body, Failure failure)
{
    if (m_done)
        return;
    m_done = true;
    m_file.close();
    m_socket->abort();
    emit finished(status, body, failure);
}

} // namespace share
