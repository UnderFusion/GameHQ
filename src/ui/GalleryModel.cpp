#include "ui/GalleryModel.h"

#include <QDateTime>
#include <QLocale>
#include <QUrl>

QString GalleryModel::formattedDate(const QString& isoDate)
{
    const QDateTime dateTime = QDateTime::fromString(isoDate, Qt::ISODate).toLocalTime();
    return dateTime.isValid() ? QLocale().toString(dateTime, QLocale::ShortFormat) : isoDate;
}

GalleryModel::GalleryModel(CaptureDatabase* db, QObject* parent)
    : QAbstractListModel(parent)
    , m_db(db)
{
    refresh();
}

int GalleryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant GalleryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size())
        return {};
    const CaptureRecord& r = m_items.at(index.row());
    switch (role) {
    case FilePathRole:  return r.filePath;
    case FileUrlRole:   return QUrl::fromLocalFile(r.filePath);
    case TypeRole:      return r.type;
    case GameNameRole:  return r.gameName;
    case DateTextRole:  return formattedDate(r.createdAt);
    case FavoriteRole:  return r.isFavorite;
    case ThumbnailRole: return r.thumbnailPath;
    case SourceRole:    return r.source;
    }
    return {};
}

QHash<int, QByteArray> GalleryModel::roleNames() const
{
    return {
        { FilePathRole,  "filePath" },
        { FileUrlRole,   "fileUrl" },
        { TypeRole,      "captureType" },
        { GameNameRole,  "gameName" },
        { DateTextRole,  "dateText" },
        { FavoriteRole,  "favorite" },
        { ThumbnailRole, "thumbnail" },
        { SourceRole,    "source" },
    };
}

void GalleryModel::setFilter(const QString& category, int gameId)
{
    m_category = category;
    m_gameId = gameId;
    refresh();
    emit filterChanged();
}

void GalleryModel::refresh()
{
    beginResetModel();
    m_items = m_db->listCaptures(m_category, m_gameId);
    endResetModel();
}

void GalleryModel::retranslate()
{
    if (!m_items.isEmpty())
        emit dataChanged(index(0), index(m_items.size() - 1), { DateTextRole });
}

void GalleryModel::toggleFavorite(int row)
{
    if (row < 0 || row >= m_items.size())
        return;
    CaptureRecord& r = m_items[row];
    if (!m_db->setFavorite(r.id, !r.isFavorite))
        return;
    r.isFavorite = !r.isFavorite;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { FavoriteRole });
    // A favorites view must drop unfavorited rows immediately.
    if (m_category == QLatin1String("favorites") && !r.isFavorite)
        refresh();
}

const CaptureRecord* GalleryModel::record(int row) const
{
    if (row < 0 || row >= m_items.size())
        return nullptr;
    return &m_items.at(row);
}

QVariantMap GalleryModel::get(int row) const
{
    const CaptureRecord* r = record(row);
    if (!r)
        return {};
    return {
        { QStringLiteral("filePath"),    r->filePath },
        { QStringLiteral("fileUrl"),     QUrl::fromLocalFile(r->filePath) },
        { QStringLiteral("captureType"), r->type },
        { QStringLiteral("gameName"),    r->gameName },
        { QStringLiteral("dateText"),    formattedDate(r->createdAt) },
        { QStringLiteral("favorite"),    r->isFavorite },
        { QStringLiteral("thumbnail"),   r->thumbnailPath },
    };
}
