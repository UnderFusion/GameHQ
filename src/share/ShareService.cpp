#include "share/ShareService.h"
#include "share/ShareSecurity.h"

#include <QDebug>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

namespace share
{

namespace
{
QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString requestErrorCode(Request::Error error)
{
    switch (error) {
    case Request::Error::None:            return {};
    case Request::Error::EmptyPath:       return QStringLiteral("no_capture");
    case Request::Error::NotFound:        return QStringLiteral("capture_missing");
    case Request::Error::NotAFile:        return QStringLiteral("capture_not_a_file");
    case Request::Error::UnsupportedType: return QStringLiteral("unsupported_media");
    case Request::Error::Empty:           return QStringLiteral("capture_empty");
    }
    return QStringLiteral("capture_missing");
}

// Only codes shaped like identifiers leave a provider; anything else (a URL,
// a server message with a token in it) is replaced wholesale.
QString sanitizedCode(const QString& code)
{
    static const QRegularExpression kCode(QStringLiteral("^[a-z][a-z0-9_]{0,47}$"));
    if (code.isEmpty() || kCode.match(code).hasMatch())
        return code;
    return QStringLiteral("provider_error");
}
} // namespace

Service::Service(QObject* parent)
    : QObject(parent)
    , m_registry(new ProviderRegistry(this))
    , m_watchdog(new QTimer(this))
{
    m_watchdog->setSingleShot(true);
    connect(m_watchdog, &QTimer::timeout, this, [this] {
        if (!m_hasActive)
            return;
        if (m_active.provider)
            m_active.provider->cancel(m_active.job.id);
        Result r;
        r.jobId = m_active.job.id;
        r.providerId = m_active.job.providerId;
        r.targetId = m_active.job.targetId;
        r.outcome = Outcome::Unconfirmed;
        r.errorCode = QStringLiteral("timeout");
        finishActive(r);
    });
    connect(m_registry, &ProviderRegistry::providersChanged, this, [this] {
        for (Provider* p : m_registry->providers())
            attach(p);
        // A provider that vanished mid-job cannot report any more.
        if (m_hasActive
            && (!m_active.provider || !m_registry->find(m_active.job.providerId))) {
            Result r;
            r.jobId = m_active.job.id;
            r.providerId = m_active.job.providerId;
            r.targetId = m_active.job.targetId;
            r.outcome = m_active.job.state == JobState::Transferring ? Outcome::Unconfirmed
                                                                     : Outcome::Failed;
            r.errorCode = QStringLiteral("provider_lost");
            finishActive(r);
        }
        emit providersChanged();
    });
}

Service::~Service() = default;

void Service::attach(Provider* provider)
{
    if (m_attached.contains(provider))
        return;
    m_attached.append(provider);
    connect(provider, &QObject::destroyed, this, [this, provider] { m_attached.removeAll(provider); });
    connect(provider, &Provider::targetsReady, this,
            [this, provider](const QString& queryId, const QVector<Target>& targets,
                             const QString& errorCode) {
                onTargetsReady(provider, queryId, targets, errorCode);
            });
    connect(provider, &Provider::jobProgress, this,
            [this, provider](const QString& jobId, JobState state, double progress) {
                onJobProgress(provider, jobId, state, progress);
            });
    connect(provider, &Provider::jobFinished, this,
            [this, provider](const Result& result) { onJobFinished(provider, result); });
}

bool Service::open(const QString& filePath, const QString& gameName)
{
    if (busy()) {
        setLastError(QStringLiteral("busy"));
        return false;
    }
    Request::Error error = Request::Error::None;
    Request request = Request::fromCapture(filePath, gameName, &error);
    if (!request.isValid()) {
        setLastError(requestErrorCode(error));
        qInfo() << "Share: cannot open capture:" << m_lastError;
        return false;
    }
    m_request = request;
    m_targets.clear();
    m_targetsProviderId.clear();
    m_pendingQueryId.clear();
    m_lastResult.clear();
    setLastError({});
    setPhase(Phase::Choosing);
    qInfo() << "Share: opened" << mediaKindName() << m_request.mimeType()
            << m_request.sizeBytes() << "bytes";
    emit sessionChanged();
    emit targetsChanged();
    emit lastResultChanged();
    return true;
}

bool Service::close()
{
    if (busy()) {
        setLastError(QStringLiteral("busy"));
        return false;
    }
    m_request = Request();
    m_targets.clear();
    m_targetsProviderId.clear();
    m_pendingQueryId.clear();
    setPhase(Phase::Idle);
    emit sessionChanged();
    emit targetsChanged();
    return true;
}

QVariantList Service::providers() const
{
    QVariantList out;
    for (Provider* p : m_registry->providersFor(m_request)) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), p->id());
        m.insert(QStringLiteral("name"), p->displayName());
        m.insert(QStringLiteral("icon"), p->iconSource());
        m.insert(QStringLiteral("available"), p->availability() == Availability::Available);
        m.insert(QStringLiteral("availability"), availabilityName(p->availability()));
        m.insert(QStringLiteral("reason"), p->availabilityReason());
        m.insert(QStringLiteral("auth"), authStateName(p->authState()));
        m.insert(QStringLiteral("capabilities"), capabilityNames(p->capabilities()));
        m.insert(QStringLiteral("access"), accountAccessName(p->accountAccess()));
        m.insert(QStringLiteral("privacy"), p->privacyNotice());
        out.append(m);
    }
    return out;
}

bool Service::requestTargets(const QString& providerId, const QString& query)
{
    Provider* p = m_registry->find(providerId);
    if (!m_request.isValid()) {
        setLastError(QStringLiteral("no_capture"));
        return false;
    }
    if (!p) {
        setLastError(QStringLiteral("unknown_provider"));
        return false;
    }
    if (p->availability() != Availability::Available) {
        setLastError(QStringLiteral("unavailable"));
        return false;
    }
    // A new query supersedes any list still on its way; answers to older ids
    // are dropped in onTargetsReady().
    m_targetsProviderId = providerId;
    m_targets.clear();
    m_pendingQueryId = newId();
    setLastError({});
    emit targetsChanged();
    p->requestTargets(m_pendingQueryId, m_request, query);
    return true;
}

QVariantList Service::targets() const
{
    QVariantList out;
    for (const Target& t : m_targets) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), t.id);
        m.insert(QStringLiteral("providerId"), t.providerId);
        m.insert(QStringLiteral("kind"), targetKindName(t.kind));
        m.insert(QStringLiteral("name"), t.displayName);
        m.insert(QStringLiteral("subtitle"), t.subtitle);
        out.append(m);
    }
    return out;
}

void Service::onTargetsReady(Provider* provider, const QString& queryId,
                             const QVector<Target>& targets, const QString& errorCode)
{
    if (queryId.isEmpty() || queryId != m_pendingQueryId || provider->id() != m_targetsProviderId)
        return;   // stale answer for a superseded query or another provider
    m_pendingQueryId.clear();
    m_targets.clear();
    const Capability needed = m_request.requiredCapability();
    for (Target t : targets) {
        // The provider cannot hand out targets in someone else's name, and a
        // target that declares media support must support this media.
        t.providerId = provider->id();
        if (t.id.isEmpty())
            continue;
        const Capabilities media = t.capabilities & (Capability::Image | Capability::Video);
        if (media != Capability::None && !media.testFlag(needed))
            continue;
        m_targets.append(t);
    }
    setLastError(sanitizedCode(errorCode));
    emit targetsChanged();
}

QString Service::dedupeKey(const QString& providerId, const QString& targetId) const
{
    return m_request.filePath().toLower() + QLatin1Char('|')
        + QString::number(m_request.sizeBytes()) + QLatin1Char('|')
        + QString::number(m_request.modifiedUtc().toMSecsSinceEpoch()) + QLatin1Char('|')
        + providerId + QLatin1Char('|') + targetId;
}

QString Service::share(const QString& providerId, const QString& targetId, bool confirmResend)
{
    const auto refuse = [this](const char* code) {
        setLastError(QString::fromLatin1(code));
        qInfo() << "Share: refused:" << m_lastError;
        return QString();
    };
    if (busy())
        return refuse("busy");
    if (!m_request.isValid())
        return refuse("no_capture");
    Provider* p = m_registry->find(providerId);
    if (!p)
        return refuse("unknown_provider");
    if (p->availability() != Availability::Available)
        return refuse("unavailable");
    if (!p->capabilities().testFlag(m_request.requiredCapability()))
        return refuse("unsupported_media");
    if (p->capabilities().testFlag(Capability::RequiresAccount)
        && p->authState() != AuthState::Connected)
        return refuse("not_connected");

    // Only a target from this provider's latest list for this capture.
    const Target* target = nullptr;
    if (m_targetsProviderId == providerId && m_pendingQueryId.isEmpty()) {
        for (const Target& t : m_targets) {
            if (t.id == targetId) {
                target = &t;
                break;
            }
        }
    }
    if (!target)
        return refuse("unknown_target");

    if (!m_request.fileUnchanged())
        return refuse("capture_changed");

    const QString key = dedupeKey(providerId, targetId);
    const auto previous = m_history.constFind(key);
    if (previous != m_history.constEnd() && !confirmResend
        && (*previous == Outcome::Sent || *previous == Outcome::Unconfirmed))
        return refuse("resend_needs_confirmation");

    m_active = ActiveJob();
    m_active.job.id = newId();
    m_active.job.requestId = m_request.id();
    m_active.job.providerId = providerId;
    m_active.job.targetId = targetId;
    m_active.job.state = JobState::Pending;
    m_active.target = *target;
    m_active.provider = p;
    m_active.dedupeKey = key;
    m_hasActive = true;
    m_lastResult.clear();
    setLastError({});
    setPhase(Phase::Sending);
    emit lastResultChanged();
    qInfo() << "Share: job" << m_active.job.id << "provider" << providerId
            << "target kind" << targetKindName(target->kind) << "media" << mediaKindName();
    m_watchdog->start(qMax(1000, p->jobTimeoutMs()));
    const Job job = m_active.job;
    const Target chosen = m_active.target;
    p->start(job, m_request, chosen);
    return job.id;
}

void Service::cancel()
{
    if (!m_hasActive)
        return;
    const bool nothingLeft = m_active.job.state == JobState::Pending
                             || m_active.job.state == JobState::Preparing;
    if (m_active.provider)
        m_active.provider->cancel(m_active.job.id);
    if (!nothingLeft)
        return;   // data may be in flight; wait for the provider's own verdict
    Result r;
    r.jobId = m_active.job.id;
    r.providerId = m_active.job.providerId;
    r.targetId = m_active.job.targetId;
    r.outcome = Outcome::Cancelled;
    finishActive(r);
}

bool Service::disconnectProvider(const QString& providerId)
{
    Provider* p = m_registry->find(providerId);
    if (!p) {
        setLastError(QStringLiteral("unknown_provider"));
        return false;
    }
    if (m_hasActive && m_active.job.providerId == providerId) {
        setLastError(QStringLiteral("busy"));
        return false;
    }
    qInfo() << "Share: disconnecting provider" << providerId;
    p->disconnectAccount();
    emit providersChanged();
    return true;
}

void Service::onJobProgress(Provider* provider, const QString& jobId, JobState state,
                            double progress)
{
    if (!m_hasActive || jobId != m_active.job.id || provider != m_active.provider)
        return;
    if (state == JobState::Finished)
        return;   // only jobFinished() ends a job
    m_active.job.state = state;
    m_active.job.progress = progress;
    // Progress proves the provider is alive; give it a fresh window.
    m_watchdog->start(qMax(1000, provider->jobTimeoutMs()));
}

void Service::onJobFinished(Provider* provider, const Result& result)
{
    if (!m_hasActive || result.jobId != m_active.job.id || provider != m_active.provider) {
        qInfo() << "Share: ignored late/foreign result from" << provider->id();
        return;
    }
    Result r = result;
    r.providerId = m_active.job.providerId;
    r.targetId = m_active.job.targetId;
    if (r.outcome == Outcome::Sent && !provider->capabilities().testFlag(Capability::DirectSend)) {
        qWarning() << "Share: provider" << provider->id()
                   << "reported sent without direct-send; recorded as handed_off";
        r.outcome = Outcome::HandedOff;
    }
    finishActive(r);
}

void Service::finishActive(Result result)
{
    if (!m_hasActive)
        return;
    m_watchdog->stop();
    m_hasActive = false;
    result.errorCode = sanitizedCode(result.errorCode);
    if (result.errorCode == QLatin1String("provider_error"))
        result.detail.clear();
    // The detail is shown to the user and may echo server text.
    result.detail = redactSecrets(result.detail);
    m_history.insert(m_active.dedupeKey, result.outcome);
    m_active.job.state = JobState::Finished;
    m_lastResult = resultToVariant(result);
    qInfo() << "Share: job" << result.jobId << "ended" << outcomeName(result.outcome)
            << (result.errorCode.isEmpty() ? QString() : result.errorCode);
    setPhase(Phase::Finished);
    emit lastResultChanged();
    emit finished(m_lastResult);
}

QVariantMap Service::resultToVariant(const Result& result)
{
    return {
        { QStringLiteral("jobId"), result.jobId },
        { QStringLiteral("providerId"), result.providerId },
        { QStringLiteral("targetId"), result.targetId },
        { QStringLiteral("outcome"), outcomeName(result.outcome) },
        { QStringLiteral("errorCode"), result.errorCode },
        { QStringLiteral("detail"), result.detail },
    };
}

QString Service::mediaKindName() const
{
    if (!m_request.isValid())
        return {};
    return m_request.mediaKind() == MediaKind::Image ? QStringLiteral("image")
                                                     : QStringLiteral("video");
}

QString Service::phaseName() const
{
    switch (m_phase) {
    case Phase::Idle:     return QStringLiteral("idle");
    case Phase::Choosing: return QStringLiteral("choosing");
    case Phase::Sending:  return QStringLiteral("sending");
    case Phase::Finished: return QStringLiteral("finished");
    }
    return QStringLiteral("idle");
}

void Service::setPhase(Phase phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    emit phaseChanged();
}

void Service::setLastError(const QString& code)
{
    if (m_lastError == code)
        return;
    m_lastError = code;
    emit lastErrorChanged();
}

} // namespace share
