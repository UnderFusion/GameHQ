#pragma once

#include <QDateTime>
#include <QFlags>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

// Share Platform core vocabulary (docs/share-platform.md). Everything here is
// plain value data: providers, the service and the QML layer exchange these
// by value and never by row index or menu position.
namespace share
{

enum class MediaKind { Image, Video };

// What a provider can do. The UI decides what to show from these flags alone,
// so a new provider never needs provider-specific QML.
enum class Capability : quint32 {
    None            = 0,
    Image           = 1u << 0,   // accepts still images (PNG/JPEG/...)
    Video           = 1u << 1,   // accepts clips (MP4/...)
    Contacts        = 1u << 2,   // targets include people
    Groups          = 1u << 3,   // targets include group chats
    Channels        = 1u << 4,   // targets include server/broadcast channels
    DirectSend      = 1u << 5,   // GameHQ itself delivers; may confirm Sent
    ExternalHandoff = 1u << 6,   // another app finishes the send; never Sent
    TargetSearch    = 1u << 7,   // listTargets() honours a query string
    Caption         = 1u << 8,   // accepts a text caption with the media
    RequiresAccount = 1u << 9,   // needs a connected account session
    SavedTargets    = 1u << 10,  // user-configured destinations (webhooks, NAS...)
};
Q_DECLARE_FLAGS(Capabilities, Capability)
Q_DECLARE_OPERATORS_FOR_FLAGS(Capabilities)

QStringList capabilityNames(Capabilities caps);

// Account/session state as the provider reports it. Desktop hand-off
// providers are NotRequired and never hold an account session.
enum class AuthState { NotRequired, Disconnected, Connecting, Connected, Error };

// Whether the provider can be offered at all right now (installed client,
// supported platform, enabled by the user).
enum class Availability { Available, NotInstalled, Unsupported, Disabled };

enum class TargetKind { Contact, Group, Channel, External, Saved };

// A destination inside one provider. `id` is opaque and only meaningful to
// the provider that issued it; the service never parses it.
struct Target
{
    QString id;
    QString providerId;
    TargetKind kind = TargetKind::External;
    QString displayName;
    QString subtitle;
    Capabilities capabilities;   // media this particular target accepts

    bool isValid() const { return !id.isEmpty() && !providerId.isEmpty(); }
};

// One explicitly chosen capture, frozen at the moment the user opened Share.
// Immutable after creation; the service re-checks the file against this
// snapshot before every job so a replaced or edited file is never sent.
class Request
{
public:
    enum class Error { None, EmptyPath, NotFound, NotAFile, UnsupportedType, Empty };

    // Validates and snapshots the file. On failure the returned request is
    // invalid and *error says why.
    static Request fromCapture(const QString& filePath, const QString& gameName,
                               Error* error = nullptr);

    bool isValid() const { return !m_id.isEmpty(); }
    const QString& id() const { return m_id; }
    const QString& filePath() const { return m_filePath; }
    const QString& fileName() const { return m_fileName; }
    MediaKind mediaKind() const { return m_mediaKind; }
    const QString& mimeType() const { return m_mimeType; }
    qint64 sizeBytes() const { return m_sizeBytes; }
    const QDateTime& modifiedUtc() const { return m_modifiedUtc; }
    const QString& gameName() const { return m_gameName; }

    // The capability a provider/target needs to accept this media.
    Capability requiredCapability() const
    {
        return m_mediaKind == MediaKind::Image ? Capability::Image : Capability::Video;
    }
    // True while the file on disk is still the one snapshotted.
    bool fileUnchanged() const;

    static QString mimeTypeForSuffix(const QString& suffix);

private:
    QString m_id;
    QString m_filePath;
    QString m_fileName;
    MediaKind m_mediaKind = MediaKind::Image;
    QString m_mimeType;
    qint64 m_sizeBytes = 0;
    QDateTime m_modifiedUtc;
    QString m_gameName;
};

enum class JobState { Pending, Preparing, Transferring, Finished };

// How a job ended. `Sent` is reserved for providers that deliver the media
// themselves AND received a confirmation; everything else must say what it
// actually knows.
enum class Outcome {
    Sent,          // provider confirmed delivery
    HandedOff,     // another app has the file; the user finishes there
    Copied,        // placed on the clipboard / a folder, nothing sent
    Failed,        // definitely not sent
    Cancelled,     // stopped by the user before anything left GameHQ
    Unconfirmed,   // may or may not have been delivered (timeout, lost link)
};

struct Result
{
    QString jobId;
    QString providerId;
    QString targetId;
    Outcome outcome = Outcome::Failed;
    // Stable machine code ("not_installed", "rate_limited", ...). Never
    // contains secrets, tokens, URLs or file contents.
    QString errorCode;
    // Short user-safe detail; same no-secrets rule.
    QString detail;
};

struct Job
{
    QString id;
    QString requestId;
    QString providerId;
    QString targetId;
    JobState state = JobState::Pending;
    double progress = -1.0;   // 0..1, or -1 when unknown
};

QString outcomeName(Outcome outcome);
QString authStateName(AuthState state);
QString availabilityName(Availability availability);
QString targetKindName(TargetKind kind);

// Provider ids are part of saved settings and the future external API:
// lowercase ASCII, digits, '.', '-' and '_', 2..64 chars, starting with a letter.
bool isValidProviderId(const QString& id);

} // namespace share

Q_DECLARE_METATYPE(share::Target)
Q_DECLARE_METATYPE(share::Result)
