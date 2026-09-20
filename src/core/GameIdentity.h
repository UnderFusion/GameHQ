#pragma once

#include <QString>

namespace GameIdentity
{
QString folderName(const QString& name);
QString key(const QString& name);
bool hasFolderForbiddenChar(const QString& name);

// Canonical executable identity for game matching (docs/mapping-presets.md
// sections 4 and 8): the one normal form game-assignment keys are written and
// looked up with, so a path spelling a caller happens to use can never create a
// second, canonically equivalent row.
//   - trimmed; case-folded to lower case;
//   - QFileInfo::canonicalFilePath() while the executable resolves (real
//     on-disk casing, separators and links), a cleaned path otherwise, so an
//     uninstalled game still normalizes deterministically;
//   - idempotent: canonical(executableKey(x)) == executableKey(x).
// The same form the runtime already uses for running executables
// (CurrentGameService::runningExecutablePaths).
QString executableKey(const QString& executablePathOrKey);

// Infers which game a capture belongs to from its location under `root`:
// "<root>/<Game>/Screenshots/shot.png" (or .../Clips/clip.mp4) and
// "<root>/<Game>/shot.png" both yield "<Game>". A file sitting directly in the
// root has no game folder to read, so it gets the same fallback name as
// folderName(). Pure path arithmetic — touches no disk.
QString inferFromPath(const QString& root, const QString& filePath);
}
