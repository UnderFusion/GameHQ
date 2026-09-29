#include "share/providers/ClipboardShareProvider.h"

#include "localization/NativeText.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QUrl>

namespace share
{

ClipboardShareProvider::ClipboardShareProvider(QObject* parent)
    : Provider(parent)
{
}

QString ClipboardShareProvider::displayName() const
{
    return NativeText::get(
        //: Share destination that copies the chosen screenshot or clip to the clipboard.
        //% "Copy to clipboard"
        QT_TRID_NOOP("gamehq.share.provider.clipboard"), "Copy to clipboard");
}

void ClipboardShareProvider::requestTargets(const QString& queryId, const Request& request,
                                            const QString& query)
{
    Q_UNUSED(request);
    Q_UNUSED(query);
    Target t;
    t.id = QStringLiteral("clipboard");
    t.kind = TargetKind::External;
    t.displayName = displayName();
    emit targetsReady(queryId, { t }, {});
}

void ClipboardShareProvider::start(const Job& job, const Request& request, const Target& target)
{
    Q_UNUSED(target);
    Result r;
    r.jobId = job.id;
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        r.outcome = Outcome::Failed;
        r.errorCode = QStringLiteral("clipboard_unavailable");
        emit jobFinished(r);
        return;
    }
    auto* mime = new QMimeData;
    mime->setUrls({ QUrl::fromLocalFile(request.filePath()) });
    if (request.mediaKind() == MediaKind::Image) {
        const QImage image(request.filePath());
        if (!image.isNull())
            mime->setImageData(image);
    }
    clipboard->setMimeData(mime);   // takes ownership
    r.outcome = Outcome::Copied;
    emit jobFinished(r);
}

} // namespace share
