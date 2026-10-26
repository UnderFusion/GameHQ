#include "updates/VersionNumber.h"

#include <QRegularExpression>
#include <climits>
#include <tuple>

namespace
{
// Leading v/V, then three dot-separated numeric components, each either "0"
// or a digit sequence with no leading zero. Nothing may trail the patch
// number, so "1.2.3-beta" and "1.2.3.4" are both rejected as malformed.
const QRegularExpression &versionPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^[vV]?(0|[1-9]\\d*)\\.(0|[1-9]\\d*)\\.(0|[1-9]\\d*)$"));
    return pattern;
}
} // namespace

VersionNumber::VersionNumber(int major, int minor, int patch)
    : m_major(major), m_minor(minor), m_patch(patch)
{
}

std::optional<VersionNumber> VersionNumber::parse(const QString &text)
{
    const QRegularExpressionMatch match = versionPattern().match(text);
    if (!match.hasMatch())
        return std::nullopt;

    bool okMajor = false;
    bool okMinor = false;
    bool okPatch = false;
    const int major = match.captured(1).toInt(&okMajor);
    const int minor = match.captured(2).toInt(&okMinor);
    const int patch = match.captured(3).toInt(&okPatch);
    if (!okMajor || !okMinor || !okPatch)
        return std::nullopt;

    return VersionNumber(major, minor, patch);
}

QString VersionNumber::toString() const
{
    return QStringLiteral("%1.%2.%3").arg(m_major).arg(m_minor).arg(m_patch);
}

bool operator==(const VersionNumber &a, const VersionNumber &b)
{
    return a.m_major == b.m_major && a.m_minor == b.m_minor && a.m_patch == b.m_patch;
}

bool operator!=(const VersionNumber &a, const VersionNumber &b)
{
    return !(a == b);
}

bool operator<(const VersionNumber &a, const VersionNumber &b)
{
    return std::tie(a.m_major, a.m_minor, a.m_patch) < std::tie(b.m_major, b.m_minor, b.m_patch);
}

bool operator>(const VersionNumber &a, const VersionNumber &b)
{
    return b < a;
}

bool operator<=(const VersionNumber &a, const VersionNumber &b)
{
    return !(b < a);
}

bool operator>=(const VersionNumber &a, const VersionNumber &b)
{
    return !(a < b);
}

ReleaseIdentity::ReleaseIdentity(VersionNumber version, int beta)
    : m_version(version), m_beta(beta)
{
}

std::optional<ReleaseIdentity> ReleaseIdentity::parse(const QString &text)
{
    static const QRegularExpression betaSuffix(QStringLiteral("-beta([1-9]\\d{0,3})$"));
    const QRegularExpressionMatch match = betaSuffix.match(text);
    const QString base = match.hasMatch() ? text.left(match.capturedStart()) : text;
    const auto version = VersionNumber::parse(base);
    if (!version.has_value())
        return std::nullopt;
    return ReleaseIdentity(*version, match.hasMatch() ? match.captured(1).toInt() : 0);
}

QString ReleaseIdentity::toString() const
{
    return m_beta > 0 ? QStringLiteral("%1-beta%2").arg(m_version.toString()).arg(m_beta)
                      : m_version.toString();
}

bool operator==(const ReleaseIdentity &a, const ReleaseIdentity &b)
{
    return a.m_version == b.m_version && a.m_beta == b.m_beta;
}

bool operator<(const ReleaseIdentity &a, const ReleaseIdentity &b)
{
    if (a.m_version != b.m_version)
        return a.m_version < b.m_version;
    // Same version: a stable release (beta 0) is newer than any of its betas.
    const int rankA = a.m_beta == 0 ? INT_MAX : a.m_beta;
    const int rankB = b.m_beta == 0 ? INT_MAX : b.m_beta;
    return rankA < rankB;
}
