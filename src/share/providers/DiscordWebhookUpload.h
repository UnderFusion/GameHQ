#pragma once

#include <QByteArray>
#include <QFile>
#include <QObject>
#include <QString>
#include <QUrl>

class QSslSocket;

namespace share
{

// One multipart/form-data POST over a socket we control, used by the Discord
// channel provider. It exists because Qt's QNetworkAccessManager silently
// re-sends a request whose connection drops before the answer, and a re-sent
// upload can post the same capture twice; Share's rule is that an ambiguous
// send is never retried. This class sends exactly once, never follows
// redirects, and reports precisely how far the request got:
//   - `requestFullySent` is true only when every byte, including the closing
//     boundary, was handed to the network;
//   - if it is false, no complete request ever reached the server, so nothing
//     can have been posted.
// Single use. Lives on the GUI thread; the file is read in small chunks as the
// socket drains, so a large clip never sits in memory and the UI never blocks.
class DiscordWebhookUpload : public QObject
{
    Q_OBJECT
public:
    enum class Failure {
        None,        // an HTTP answer arrived (see status)
        Connect,     // could not reach the host (DNS, refused, timeout)
        Tls,         // certificate or handshake problem
        Io,          // connection dropped / unreadable file mid-way
        Aborted,     // abort() was called
    };

    struct Part
    {
        QString path;       // capture file
        QString fileName;   // sent as the attachment name
        QString mimeType;
    };

    explicit DiscordWebhookUpload(QObject* parent = nullptr);
    ~DiscordWebhookUpload() override;

    // `endpoint` is the full URL including the query. https, or http for
    // tests. Emits finished() exactly once.
    void start(const QUrl& endpoint, const QByteArray& payloadJson, const Part& file);
    void abort();

    bool requestFullySent() const { return m_fullySent; }
    qint64 bytesSent() const { return m_written; }
    qint64 bytesTotal() const { return m_total; }

signals:
    void progress(qint64 sent, qint64 total);
    void finished(int status, const QByteArray& body, DiscordWebhookUpload::Failure failure);

private:
    void begin();
    void pump();
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    void complete(int status, const QByteArray& body, Failure failure);
    bool parseResponse(bool connectionClosed);

    QSslSocket* m_socket = nullptr;
    QUrl m_url;
    QByteArray m_head;        // request line, headers and the multipart preamble
    QByteArray m_tail;        // closing boundary
    QFile m_file;
    qint64 m_fileLeft = 0;
    qint64 m_total = 0;       // bytes of the whole request (headers + body)
    qint64 m_written = 0;
    bool m_queuedAll = false;
    bool m_fullySent = false;
    bool m_done = false;
    bool m_secure = true;
    bool m_started = false;   // handshake done, request bytes are flowing
    QByteArray m_response;
    int m_status = 0;
    bool m_headerParsed = false;
    qint64 m_contentLength = -1;
    bool m_chunked = false;
    qsizetype m_bodyStart = 0;
};

} // namespace share
