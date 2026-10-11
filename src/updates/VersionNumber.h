#pragma once

#include <QString>
#include <optional>

// Strict major.minor.patch version parsing and comparison for update checks.
// Versions are NEVER compared as strings ("9" < "10" lexically, but 9 < 10
// numerically is what every caller actually wants).
class VersionNumber
{
public:
    // Accepts "1.2.3" or "v1.2.3" / "V1.2.3". Each component must be "0" or
    // a digit sequence with no leading zero, and nothing may follow the
    // patch number (no build metadata, no prerelease suffix). Returns
    // std::nullopt for anything else.
    static std::optional<VersionNumber> parse(const QString &text);

    int major() const { return m_major; }
    int minor() const { return m_minor; }
    int patch() const { return m_patch; }

    QString toString() const;

    friend bool operator==(const VersionNumber &a, const VersionNumber &b);
    friend bool operator!=(const VersionNumber &a, const VersionNumber &b);
    friend bool operator<(const VersionNumber &a, const VersionNumber &b);
    friend bool operator>(const VersionNumber &a, const VersionNumber &b);
    friend bool operator<=(const VersionNumber &a, const VersionNumber &b);
    friend bool operator>=(const VersionNumber &a, const VersionNumber &b);

private:
    VersionNumber(int major, int minor, int patch);

    int m_major = 0;
    int m_minor = 0;
    int m_patch = 0;
};

// One published GameHQ build: a product version plus, for a GitHub
// pre-release, its beta number. Every beta of one version reports the same
// VERSION, so the beta number is what tells them apart. A stable release
// outranks every beta of the same version: 0.7.10-beta1 < 0.7.10-beta2 <
// 0.7.10 < 0.7.11-beta1.
class ReleaseIdentity
{
public:
    // Accepts "1.2.3" / "v1.2.3" (stable) and "1.2.3-betaN" / "v1.2.3-betaN"
    // with N >= 1 and no leading zero. Anything else is std::nullopt.
    static std::optional<ReleaseIdentity> parse(const QString &text);

    const VersionNumber &version() const { return m_version; }
    int beta() const { return m_beta; }   // 0 for a stable release
    bool isBeta() const { return m_beta > 0; }

    // "0.7.10" or "0.7.10-beta2", without the leading v.
    QString toString() const;

    friend bool operator==(const ReleaseIdentity &a, const ReleaseIdentity &b);
    friend bool operator<(const ReleaseIdentity &a, const ReleaseIdentity &b);
    friend bool operator>(const ReleaseIdentity &a, const ReleaseIdentity &b) { return b < a; }
    friend bool operator<=(const ReleaseIdentity &a, const ReleaseIdentity &b) { return !(b < a); }

private:
    ReleaseIdentity(VersionNumber version, int beta);

    VersionNumber m_version;
    int m_beta = 0;
};
