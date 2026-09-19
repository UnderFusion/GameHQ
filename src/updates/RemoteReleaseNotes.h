#pragma once
#include <QObject>
#include <QString>
#include <QVariantList>

class QNetworkAccessManager;

// Presentation-only HTTPS content; never used to authorize an update.
class RemoteReleaseNotes final : public QObject
{
    Q_OBJECT
public:
    RemoteReleaseNotes(QString owner, QString repo, QString cacheRoot,
                       QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    void select(const QString &version, const QString &tag, const QString &body,
                const QString &locale);
    void retry();
    QString markdown() const { return m_markdown; }
    QVariantList blocks() const;
    bool loading() const { return m_loading; }
Q_SIGNALS:
    void changed();
private:
    void fetch(const QString &locale, quint64 generation);
    void present(const QString &markdown);
    QString cachePath(const QString &locale) const;
    bool validDocument(const QByteArray &bytes) const;
    QNetworkAccessManager *m_network;
    QString m_owner, m_repo, m_cacheRoot;
    QString m_version, m_tag, m_body, m_locale, m_markdown;
    quint64 m_generation = 0;
    bool m_loading = false;
};
