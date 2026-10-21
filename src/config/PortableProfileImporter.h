#pragma once

#include <QString>

class PortableProfileImporter
{
public:
    enum class FailurePoint {
        None,
        AfterStaging,
        AfterDatabaseRewrite,
        BeforePublish,
        AfterDestinationBackup,
        AfterPublish,
        InterruptAfterDestinationBackup,
        InterruptAfterPublish
    };

    struct Options {
        QString sourcePackageRoot;
        QString destinationDataRoot;
        FailurePoint failurePoint = FailurePoint::None;
    };

    struct Result {
        int captures = 0;
        int games = 0;
        int watchedFolders = 0;
        int copiedSounds = 0;
        QString evidencePath;
        // Set when the import succeeded but leftover cleanup did not; the
        // next start finishes it.
        QString warning;
    };

    // Imports an immutable portable profile into a new/empty installed profile.
    // The failure hook is public only so disposable tests can prove rollback.
    static bool importProfile(const Options& options, Result& result, QString& error);
};
