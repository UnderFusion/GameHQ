#pragma once

#include <QSqlDatabase>

// Scoped QSqlDatabase transaction. begin() opens it; every exit that has not
// committed successfully rolls back when the guard leaves scope, including a
// failed commit(). Logging stays with the caller, so a failure path logs first
// and the rollback follows on return - the same order the hand-written
// "log; rollback(); return" blocks used.
class SqlTransaction
{
public:
    explicit SqlTransaction(QSqlDatabase& db) : m_db(db) {}
    ~SqlTransaction()
    {
        if (m_open)
            m_db.rollback();
    }

    SqlTransaction(const SqlTransaction&) = delete;
    SqlTransaction& operator=(const SqlTransaction&) = delete;

    bool begin()
    {
        m_open = m_db.transaction();
        return m_open;
    }

    // A failed commit leaves the transaction open, so the destructor rolls it back.
    bool commit()
    {
        if (!m_db.commit())
            return false;
        m_open = false;
        return true;
    }

private:
    QSqlDatabase& m_db;
    bool m_open = false;
};
