#pragma once
#include <QAbstractListModel>
#include <QVariantMap>
#include "storage/CaptureDatabase.h"

// List model over CaptureDatabase for the QML gallery grid.
class GalleryModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString category READ category NOTIFY filterChanged)
    Q_PROPERTY(int gameId READ gameId NOTIFY filterChanged)
public:
    enum Roles {
        FilePathRole = Qt::UserRole + 1,
        FileUrlRole,
        TypeRole,
        GameNameRole,
        DateTextRole,
        FavoriteRole,
        ThumbnailRole,
        SourceRole,
    };

    explicit GalleryModel(CaptureDatabase* db, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // category: all | recent | favorites | screenshots | clips; gameId ≥ 0 filters.
    // Q_INVOKABLE so a second instance (the overlay's own GalleryModel, kept
    // separate from the main window's filter state) can be driven directly
    // from QML instead of through AppController.
    Q_INVOKABLE void setFilter(const QString& category, int gameId = -1);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void retranslate();
    Q_INVOKABLE void toggleFavorite(int row);
    const CaptureRecord* record(int row) const;

    // Convenience accessor for QML code outside a delegate context (e.g. the
    // lightbox viewer), which cannot call the protected data()/index() pair
    // by role id directly.
    Q_INVOKABLE QVariantMap get(int row) const;

    QString category() const { return m_category; }
    int gameId() const { return m_gameId; }

signals:
    void filterChanged();

private:
    static QString formattedDate(const QString& isoDate);
    CaptureDatabase* m_db;
    QString m_category = QStringLiteral("all");
    int m_gameId = -1;
    QVector<CaptureRecord> m_items;
};
