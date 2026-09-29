#pragma once

#include "share/ShareProvider.h"

namespace share
{

// Built-in, offline destination: puts the chosen capture on the Windows
// clipboard as a file (pastes into Explorer, Telegram, Discord...) and, for a
// screenshot, also as image data. Nothing leaves the PC, so the outcome is
// always `copied`, never `sent`.
class ClipboardShareProvider : public Provider
{
    Q_OBJECT
public:
    explicit ClipboardShareProvider(QObject* parent = nullptr);

    QString id() const override { return QStringLiteral("clipboard"); }
    QString displayName() const override;
    QString iconSource() const override { return QStringLiteral(""); }   // Segoe Fluent: Copy
    Capabilities capabilities() const override
    {
        return Capability::Image | Capability::Video;
    }
    int jobTimeoutMs() const override { return 10000; }

    void requestTargets(const QString& queryId, const Request& request,
                        const QString& query) override;
    void start(const Job& job, const Request& request, const Target& target) override;
};

} // namespace share
