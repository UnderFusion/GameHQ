#pragma once

#include <QtGlobal>

// GUI-thread ownership, independent of a particular worker/pump generation.
// Completion tokens prevent an old or rejected request from releasing a newer one.
class ReplayBufferOwners
{
public:
    enum Owner { Auto = 1, ManualSave = 2, WindowScreenshot = 4 };
    unsigned mask() const { return (m_auto ? Auto : 0) | (m_manual ? ManualSave : 0)
                                  | (m_screenshot ? WindowScreenshot : 0); }
    bool owns(Owner owner) const { return (mask() & owner) != 0; }
    bool needsSession() const { return mask() != 0; }
    bool hasExplicitOwner() const { return m_manual || m_screenshot; }
    bool savePending() const { return m_savePending; }
    quint64 manualToken() const { return m_manual; }
    void setAuto(bool enabled) { m_auto = enabled; }

    quint64 acquireManual(qint64 nowMs, int idleSeconds)
    {
        if (m_savePending)
            return 0;
        if (!m_manual)
            m_manual = ++m_sequence;
        m_idleDeadline = nowMs + qint64(qBound(10, idleSeconds, 600)) * 1000;
        return m_manual;
    }
    bool beginSave(quint64 token)
    {
        if (!token || token != m_manual || m_savePending)
            return false;
        m_savePending = true;
        return true;
    }
    bool finishSave(quint64 token)
    {
        if (!token || token != m_manual || !m_savePending)
            return false;
        m_savePending = false;
        m_manual = 0;
        return true;
    }
    bool releaseColdManual()
    {
        if (!m_manual || m_savePending)
            return false;
        m_manual = 0;
        return true;
    }
    bool expireIdle(qint64 nowMs)
    {
        return nowMs >= m_idleDeadline && releaseColdManual();
    }
    quint64 acquireScreenshot()
    {
        if (m_screenshot)
            return 0;
        return m_screenshot = ++m_sequence;
    }
    bool finishScreenshot(quint64 token)
    {
        if (!token || token != m_screenshot)
            return false;
        m_screenshot = 0;
        return true;
    }

private:
    bool m_auto = false;
    bool m_savePending = false;
    quint64 m_sequence = 0;
    quint64 m_manual = 0;
    quint64 m_screenshot = 0;
    qint64 m_idleDeadline = 0;
};
