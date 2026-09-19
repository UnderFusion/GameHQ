#pragma once

#include <QDateTime>
#include <QString>

// Immutable snapshot of one GitHub release, already filtered down to the
// fields the update checker needs. Never carries an access token.
//
// Everything here is UNTRUSTED discovery metadata. GitHub tells GameHQ where
// the assets are; the signed manifest decides whether any of them may be
// installed (docs/release-manifest-security-review.md).
struct ReleaseInfo
{
    QString version;      // Tag name, expected to equal the VERSION file exactly.
    QString tag;           // Exact GitHub tag for version-pinned presentation content.
    QString name;          // Release title.
    QString notes;         // Release body (raw Markdown/plain text from GitHub).
    QDateTime publishedAt;
    QString webUrl;        // Human-facing GitHub release page.
    QString zipUrl;        // Download URL of the "-win64-update.zip" asset.
    QString zipName;
    qint64 zipSize = 0;
    QString checksumUrl;   // ".sha256" asset. Human convenience only; never a trust root.
    QString manifestUrl;   // Download URL of "gamehq-release.json".
    QString signatureUrl;  // Download URL of "gamehq-release.sig".
    bool prerelease = false;
    bool draft = false;

    // A freshly published release may still be uploading its assets; such a
    // release must not be offered and its ETag must not be cached (a cached
    // ETag would answer 304 forever and hide the completed assets).
    //
    // Without both manifest assets there is nothing that could authorise an
    // install, so such a release is not installable either.
    bool hasCompleteUpdateAssets() const
    {
        return !zipName.isEmpty() && !zipUrl.isEmpty() && zipSize > 0
            && !manifestUrl.isEmpty() && !signatureUrl.isEmpty();
    }
};

// Everything the installer needs to prove that a staged package was authorised
// by a signed release manifest. Produced only after UpdateDownloader has
// verified the signature over the exact manifest bytes and hashed the package.
struct VerifiedUpdate
{
    QString packagePath;
    QByteArray packageSha256;   // raw digest bytes of the downloaded archive
    QString version;
    QString manifestPath;       // saved copy of the exact signed bytes
    QString signaturePath;      // saved copy of the detached signature
    QString manifestSha256;     // lowercase hex of the signed bytes
    QString signature;          // canonical padded Base64
    QString keyId;
    quint64 releaseSequence = 0;
    QString artifactName;
    qint64 artifactSize = 0;
    QString artifactSha256;     // lowercase hex from the manifest

    bool isValid() const
    {
        return !packagePath.isEmpty() && packageSha256.size() == 32 && !version.isEmpty()
            && !manifestPath.isEmpty() && !signaturePath.isEmpty()
            && manifestSha256.size() == 64 && !signature.isEmpty() && !keyId.isEmpty()
            && releaseSequence > 0 && !artifactName.isEmpty() && artifactSize > 0
            && artifactSha256.size() == 64;
    }
};
