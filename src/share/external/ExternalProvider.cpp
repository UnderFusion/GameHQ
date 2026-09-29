#include "share/external/ExternalProvider.h"

#include "localization/NativeText.h"

#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

namespace share::external
{

namespace
{
constexpr int kDefaultTargetsTimeoutMs = 10000;

QString newToken()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool parseKind(const QString& text, TargetKind* kind)
{
    if (text.isEmpty() || text == QLatin1String("external")) {
        *kind = TargetKind::External;
    } else if (text == QLatin1String("contact")) {
        *kind = TargetKind::Contact;
    } else if (text == QLatin1String("group")) {
        *kind = TargetKind::Group;
    } else if (text == QLatin1String("channel")) {
        *kind = TargetKind::Channel;
    } else {
        return false;
    }
    return true;
}

bool parseOutcome(const QString& text, Outcome* outcome)
{
    if (text == QLatin1String("sent"))            *outcome = Outcome::Sent;
    else if (text == QLatin1String("handed_off")) *outcome = Outcome::HandedOff;
    else if (text == QLatin1String("copied"))     *outcome = Outcome::Copied;
    else if (text == QLatin1String("failed"))     *outcome = Outcome::Failed;
    else if (text == QLatin1String("cancelled"))  *outcome = Outcome::Cancelled;
    else if (text == QLatin1String("unconfirmed")) *outcome = Outcome::Unconfirmed;
    else return false;
    return true;
}

// Codes leaving a provider are lowercase identifiers only.
bool isCode(const QString& text)
{
    static const QRegularExpression kCode(QStringLiteral("^[a-z][a-z0-9_]{0,47}$"));
    return kCode.match(text).hasMatch();
}
} // namespace

ExternalProvider::ExternalProvider(Manifest manifest, Sender send, QObject* parent)
    : Provider(parent)
    , m_manifest(std::move(manifest))
    , m_send(std::move(send))
    , m_targetsTimer(new QTimer(this))
{
    m_targetsTimer->setSingleShot(true);
    m_targetsTimer->setInterval(kDefaultTargetsTimeoutMs);
    connect(m_targetsTimer, &QTimer::timeout, this, [this] {
        if (m_pendingQuery.isEmpty())
            return;
        const QString query = m_pendingQuery;
        m_pendingQuery.clear();
        emit targetsReady(query, {}, QStringLiteral("timeout"));
    });
}

void ExternalProvider::setTargetsTimeoutMs(int ms)
{
    m_targetsTimer->setInterval(qMax(50, ms));
}

QString ExternalProvider::availabilityReason() const
{
    if (!m_lost)
        return {};
    return NativeText::get(
        //: Share destination is disabled because its add-on program disconnected.
        //% "This add-on is not running."
        QT_TRID_NOOP("gamehq.share.external.not_running"), "This add-on is not running.");
}

QString ExternalProvider::privacyNotice() const
{
    // Fixed, GameHQ-authored wording first: the provider's own text is only a
    // supplement and can never replace the disclosure that it is third-party.
    QString notice = NativeText::get(
        //: Shown under a Share destination provided by another program on this PC.
        //% "Add-on from another program on this PC. It receives the capture you choose to share."
        QT_TRID_NOOP("gamehq.share.external.privacy"),
        "Add-on from another program on this PC. It receives the capture you choose to share.");
    if (!m_manifest.privacy.isEmpty())
        notice += QLatin1Char(' ') + m_manifest.privacy;
    return notice;
}

void ExternalProvider::requestTargets(const QString& queryId, const Request& request,
                                      const QString& query)
{
    if (m_lost) {
        emit targetsReady(queryId, {}, QStringLiteral("provider_unavailable"));
        return;
    }
    m_pendingQuery = queryId;
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("targets.request"));
    msg.insert(QStringLiteral("requestId"), queryId);
    // A provider without target_search never sees the user's search text.
    msg.insert(QStringLiteral("query"),
               m_manifest.capabilities.testFlag(Capability::TargetSearch)
                   ? sanitizeText(query, kMaxQueryChars)
                   : QString());
    msg.insert(QStringLiteral("mediaKind"), request.mediaKind() == MediaKind::Image
                                                ? QStringLiteral("image")
                                                : QStringLiteral("video"));
    if (!m_send(msg)) {
        m_pendingQuery.clear();
        emit targetsReady(queryId, {}, QStringLiteral("provider_unavailable"));
        return;
    }
    m_targetsTimer->start();
}

void ExternalProvider::start(const Job& job, const Request& request, const Target& target)
{
    Result r;
    r.jobId = job.id;
    if (m_lost || !m_jobId.isEmpty()) {
        r.outcome = Outcome::Failed;
        r.errorCode = QStringLiteral("provider_unavailable");
        emit jobFinished(r);
        return;
    }
    m_jobId = job.id;
    m_jobToken = newToken();
    m_cancelSent = false;

    // The one place a path leaves GameHQ: the capture the user chose in this
    // user-initiated Share, described with the metadata GameHQ verified.
    QJsonObject file;
    file.insert(QStringLiteral("path"), QDir::toNativeSeparators(request.filePath()));
    file.insert(QStringLiteral("fileName"), request.fileName());
    file.insert(QStringLiteral("sizeBytes"), double(request.sizeBytes()));
    file.insert(QStringLiteral("mimeType"), request.mimeType());
    file.insert(QStringLiteral("mediaKind"), request.mediaKind() == MediaKind::Image
                                                 ? QStringLiteral("image")
                                                 : QStringLiteral("video"));
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("job.start"));
    msg.insert(QStringLiteral("jobId"), m_jobId);
    msg.insert(QStringLiteral("token"), m_jobToken);
    msg.insert(QStringLiteral("targetId"), target.id);
    msg.insert(QStringLiteral("file"), file);
    msg.insert(QStringLiteral("gameName"), sanitizeText(request.gameName(), 96));

    emit jobProgress(job.id, JobState::Preparing, -1.0);
    if (!m_send(msg)) {
        clearJob();
        r.outcome = Outcome::Failed;
        r.errorCode = QStringLiteral("provider_unavailable");
        emit jobFinished(r);
    }
}

void ExternalProvider::cancel(const QString& jobId)
{
    if (m_jobId.isEmpty() || jobId != m_jobId || m_cancelSent || m_lost)
        return;
    m_cancelSent = true;
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("job.cancel"));
    msg.insert(QStringLiteral("jobId"), m_jobId);
    msg.insert(QStringLiteral("token"), m_jobToken);
    m_send(msg);   // the provider answers with job.result; the watchdog covers silence
}

void ExternalProvider::clearJob()
{
    m_jobId.clear();
    m_jobToken.clear();
    m_cancelSent = false;
}

bool ExternalProvider::matchesActiveJob(const QJsonObject& message) const
{
    return !m_jobId.isEmpty()
        && message.value(QStringLiteral("jobId")).toString() == m_jobId
        && message.value(QStringLiteral("token")).toString() == m_jobToken;
}

QString ExternalProvider::handleMessage(const QString& type, const QJsonObject& message)
{
    if (type == QLatin1String("targets.result"))
        return onTargetsResult(message);
    if (type == QLatin1String("job.progress"))
        return onJobProgress(message);
    if (type == QLatin1String("job.result"))
        return onJobResult(message);
    return QString::fromLatin1(ErrorCode::UnknownType);
}

QString ExternalProvider::onTargetsResult(const QJsonObject& message)
{
    const QJsonValue reqV = message.value(QStringLiteral("requestId"));
    if (!reqV.isString())
        return QString::fromLatin1(ErrorCode::InvalidField);
    // An answer to a superseded or timed-out query is harmless: drop it.
    if (m_pendingQuery.isEmpty() || reqV.toString() != m_pendingQuery)
        return {};

    QString errorCode;
    if (message.contains(QStringLiteral("error"))) {
        const QString code = message.value(QStringLiteral("error")).toString();
        errorCode = isCode(code) ? code : QStringLiteral("provider_error");
    }
    const QJsonValue listV = message.value(QStringLiteral("targets"));
    if (!listV.isUndefined() && !listV.isArray())
        return QString::fromLatin1(ErrorCode::InvalidField);

    QVector<Target> targets;
    QStringList seen;
    for (const QJsonValue& v : listV.toArray()) {
        if (targets.size() >= kMaxTargets)
            break;   // bounded: the rest is dropped, not an error
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        const QString id = o.value(QStringLiteral("id")).toString();
        const QString name = sanitizeText(o.value(QStringLiteral("name")).toString(), kMaxTargetNameChars);
        TargetKind kind = TargetKind::External;
        if (!isPlainId(id, kMaxTargetIdChars) || name.isEmpty() || seen.contains(id)
            || !parseKind(o.value(QStringLiteral("kind")).toString(), &kind))
            continue;   // one bad row never poisons the list
        seen.append(id);
        Target t;
        t.id = id;
        t.kind = kind;
        t.displayName = name;
        t.subtitle = sanitizeText(o.value(QStringLiteral("subtitle")).toString(), kMaxTargetSubtitleChars);
        // Per-target media support can only narrow what the manifest declared.
        Capabilities media;
        for (const QJsonValue& m : o.value(QStringLiteral("media")).toArray()) {
            const Capability c = capabilityFromName(m.toString());
            if ((c == Capability::Image || c == Capability::Video) && m_manifest.capabilities.testFlag(c))
                media |= c;
        }
        // A target that lists media but none the provider declared accepts
        // nothing; leaving it "unrestricted" would widen the manifest.
        if (o.contains(QStringLiteral("media")) && media == Capabilities())
            continue;
        t.capabilities = media;
        targets.append(t);
    }
    m_targetsTimer->stop();
    const QString query = m_pendingQuery;
    m_pendingQuery.clear();
    emit targetsReady(query, targets, errorCode);
    return {};
}

QString ExternalProvider::onJobProgress(const QJsonObject& message)
{
    if (!matchesActiveJob(message))
        return QString::fromLatin1(ErrorCode::StaleJob);
    const QString state = message.value(QStringLiteral("state")).toString();
    JobState js;
    if (state == QLatin1String("preparing"))
        js = JobState::Preparing;
    else if (state == QLatin1String("transferring"))
        js = JobState::Transferring;
    else
        return QString::fromLatin1(ErrorCode::InvalidField);
    double progress = -1.0;
    if (message.contains(QStringLiteral("progress"))) {
        const QJsonValue p = message.value(QStringLiteral("progress"));
        if (!p.isDouble())
            return QString::fromLatin1(ErrorCode::InvalidField);
        progress = qBound(-1.0, p.toDouble(), 1.0);
    }
    emit jobProgress(m_jobId, js, progress);
    return {};
}

QString ExternalProvider::onJobResult(const QJsonObject& message)
{
    if (!matchesActiveJob(message))
        return QString::fromLatin1(ErrorCode::StaleJob);
    Outcome outcome;
    if (!parseOutcome(message.value(QStringLiteral("outcome")).toString(), &outcome))
        return QString::fromLatin1(ErrorCode::InvalidField);
    Result r;
    r.jobId = m_jobId;
    r.outcome = outcome;
    if (message.contains(QStringLiteral("errorCode"))) {
        const QString code = message.value(QStringLiteral("errorCode")).toString();
        r.errorCode = isCode(code) ? code : QStringLiteral("provider_error");
    }
    r.detail = sanitizeText(message.value(QStringLiteral("detail")).toString(), kMaxDetailChars);
    clearJob();   // the token dies with the job
    emit jobFinished(r);
    return {};
}

void ExternalProvider::connectionLost()
{
    if (m_lost)
        return;
    m_lost = true;
    m_targetsTimer->stop();
    if (!m_pendingQuery.isEmpty()) {
        const QString query = m_pendingQuery;
        m_pendingQuery.clear();
        emit targetsReady(query, {}, QStringLiteral("provider_unavailable"));
    }
    if (!m_jobId.isEmpty()) {
        // job.start was sent, so the peer may have delivered before dying.
        Result r;
        r.jobId = m_jobId;
        r.outcome = Outcome::Unconfirmed;
        r.errorCode = QStringLiteral("provider_disconnected");
        clearJob();
        emit jobFinished(r);
    }
    emit stateChanged();
}

} // namespace share::external
