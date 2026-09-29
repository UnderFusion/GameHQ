#include "share/ShareTypes.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>

namespace share
{

QStringList capabilityNames(Capabilities caps)
{
    struct Name { Capability flag; const char* name; };
    static const Name kNames[] = {
        { Capability::Image, "image" },
        { Capability::Video, "video" },
        { Capability::Contacts, "contacts" },
        { Capability::Groups, "groups" },
        { Capability::Channels, "channels" },
        { Capability::DirectSend, "direct-send" },
        { Capability::ExternalHandoff, "external-handoff" },
        { Capability::TargetSearch, "target-search" },
        { Capability::Caption, "caption" },
        { Capability::RequiresAccount, "requires-account" },
        { Capability::SavedTargets, "saved-targets" },
    };
    QStringList out;
    for (const Name& n : kNames) {
        if (caps.testFlag(n.flag))
            out.append(QString::fromLatin1(n.name));
    }
    return out;
}

QString Request::mimeTypeForSuffix(const QString& suffix)
{
    const QString s = suffix.toLower();
    if (s == QLatin1String("png"))  return QStringLiteral("image/png");
    if (s == QLatin1String("jpg") || s == QLatin1String("jpeg")) return QStringLiteral("image/jpeg");
    if (s == QLatin1String("webp")) return QStringLiteral("image/webp");
    if (s == QLatin1String("bmp"))  return QStringLiteral("image/bmp");
    if (s == QLatin1String("mp4"))  return QStringLiteral("video/mp4");
    if (s == QLatin1String("mov"))  return QStringLiteral("video/quicktime");
    if (s == QLatin1String("mkv"))  return QStringLiteral("video/x-matroska");
    if (s == QLatin1String("webm")) return QStringLiteral("video/webm");
    return {};
}

Request Request::fromCapture(const QString& filePath, const QString& gameName, Error* error)
{
    const auto fail = [&](Error e) {
        if (error)
            *error = e;
        return Request();
    };
    if (filePath.trimmed().isEmpty())
        return fail(Error::EmptyPath);
    const QFileInfo info(filePath);
    if (!info.exists())
        return fail(Error::NotFound);
    // A symlink or junction could point the provider at a different file
    // than the one the user picked; only plain files are shareable.
    if (!info.isFile() || info.isSymLink() || info.isJunction())
        return fail(Error::NotAFile);
    const QString mime = mimeTypeForSuffix(info.suffix());
    if (mime.isEmpty())
        return fail(Error::UnsupportedType);
    if (info.size() <= 0)
        return fail(Error::Empty);

    Request r;
    r.m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    r.m_filePath = info.absoluteFilePath();
    r.m_fileName = info.fileName();
    r.m_mimeType = mime;
    r.m_mediaKind = mime.startsWith(QLatin1String("video/")) ? MediaKind::Video : MediaKind::Image;
    r.m_sizeBytes = info.size();
    r.m_modifiedUtc = info.lastModified().toUTC();
    r.m_gameName = gameName;
    if (error)
        *error = Error::None;
    return r;
}

bool Request::fileUnchanged() const
{
    if (!isValid())
        return false;
    const QFileInfo info(m_filePath);
    return info.isFile() && !info.isSymLink() && info.size() == m_sizeBytes
        && info.lastModified().toUTC() == m_modifiedUtc;
}

QString outcomeName(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Sent:        return QStringLiteral("sent");
    case Outcome::HandedOff:   return QStringLiteral("handed_off");
    case Outcome::Copied:      return QStringLiteral("copied");
    case Outcome::Failed:      return QStringLiteral("failed");
    case Outcome::Cancelled:   return QStringLiteral("cancelled");
    case Outcome::Unconfirmed: return QStringLiteral("unconfirmed");
    }
    return QStringLiteral("failed");
}

QString authStateName(AuthState state)
{
    switch (state) {
    case AuthState::NotRequired:  return QStringLiteral("not_required");
    case AuthState::Disconnected: return QStringLiteral("disconnected");
    case AuthState::Connecting:   return QStringLiteral("connecting");
    case AuthState::Connected:    return QStringLiteral("connected");
    case AuthState::Error:        return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

QString availabilityName(Availability availability)
{
    switch (availability) {
    case Availability::Available:    return QStringLiteral("available");
    case Availability::NotInstalled: return QStringLiteral("not_installed");
    case Availability::Unsupported:  return QStringLiteral("unsupported");
    case Availability::Disabled:     return QStringLiteral("disabled");
    }
    return QStringLiteral("unsupported");
}

QString targetKindName(TargetKind kind)
{
    switch (kind) {
    case TargetKind::Contact:  return QStringLiteral("contact");
    case TargetKind::Group:    return QStringLiteral("group");
    case TargetKind::Channel:  return QStringLiteral("channel");
    case TargetKind::External: return QStringLiteral("external");
    case TargetKind::Saved:    return QStringLiteral("saved");
    }
    return QStringLiteral("external");
}

bool isValidProviderId(const QString& id)
{
    static const QRegularExpression kPattern(QStringLiteral("^[a-z][a-z0-9._-]{1,63}$"));
    return kPattern.match(id).hasMatch();
}

} // namespace share
