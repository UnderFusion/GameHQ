#include "notify/ToastModel.h"

QVariant ToastModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const auto& t = m_rows.at(index.row());
    switch (role) {
    case Key: return t.key;
    case Title: return t.title;
    case Body: return t.body;
    case ImageUrl: return t.imageUrl;
    case Kind: return t.kind;
    case When: return t.when;
    case Video: return t.video;
    case Pending: return t.pending;
    case Revision: return t.revision;
    default: return {};
    }
}
QHash<int, QByteArray> ToastModel::roleNames() const
{
    return {{Key,"toastKey"},{Title,"title"},{Body,"body"},{ImageUrl,"imageUrl"},
            {Kind,"kind"},{When,"when"},{Video,"isVideo"},{Pending,"pending"},{Revision,"revision"}};
}
int ToastModel::find(const QString& key) const
{
    for (int i = 0; i < m_rows.size(); ++i) if (m_rows.at(i).key == key) return i;
    return -1;
}
void ToastModel::setLimit(int limit)
{
    m_limit = qMax(0, limit);
    if (m_rows.size() <= m_limit) return;
    const int remove = m_rows.size() - m_limit;
    beginRemoveRows({}, 0, remove - 1);
    m_rows.remove(0, remove);
    endRemoveRows();
}
bool ToastModel::replace(int row, Toast t)
{
    const auto& old = m_rows.at(row);
    // Duplicate request receipt cannot turn a terminal outcome back into pending.
    if (!old.pending && t.pending) return false;
    if (old.title == t.title && old.body == t.body && old.imageUrl == t.imageUrl
        && old.kind == t.kind && old.when == t.when && old.video == t.video && old.pending == t.pending)
        return false;
    t.revision = ++m_revision;
    m_rows[row] = t;
    emit dataChanged(index(row), index(row));
    return true;
}
bool ToastModel::post(Toast t)
{
    const int row = find(t.key);
    if (row >= 0) return replace(row, t);
    if (m_limit <= 0) return false;
    if (m_rows.size() >= m_limit) {
        beginRemoveRows({}, 0, 0); m_rows.removeFirst(); endRemoveRows();
    }
    const int end = m_rows.size();
    beginInsertRows({}, end, end);
    t.revision = ++m_revision;
    m_rows.append(t);
    endInsertRows();
    return true;
}
bool ToastModel::update(Toast t)
{
    const int row = find(t.key);
    if (row < 0) return false; // Never resurrect an expired/evicted operation.
    replace(row, t);
    return true; // An identical update is accepted without restarting presentation.
}
bool ToastModel::dismiss(const QString& key, int revision)
{
    const int row = find(key);
    if (row < 0 || m_rows.at(row).revision != revision) return false;
    beginRemoveRows({}, row, row); m_rows.removeAt(row); endRemoveRows();
    return true;
}
