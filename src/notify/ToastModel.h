#pragma once
#include <QAbstractListModel>
#include <QDateTime>
#include <QVector>

// Visible presentation only. Removing a row does not touch capture/history data.
class ToastModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { Key = Qt::UserRole + 1, Title, Body, ImageUrl, Kind, When, Video, Pending, Revision };
    struct Toast {
        QString key, title, body, imageUrl, kind;
        QDateTime when;
        bool video = false, pending = false;
        int revision = 0;
    };
    explicit ToastModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : m_rows.size(); }
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int limit() const { return m_limit; }
    void setLimit(int limit);
    bool post(Toast toast);
    bool update(Toast toast);
    bool dismiss(const QString& key, int revision);
private:
    int find(const QString& key) const;
    bool replace(int row, Toast toast);
    QVector<Toast> m_rows;
    int m_limit = 0; // Theme supplies the cap before the first UI post.
    int m_revision = 0;
};
