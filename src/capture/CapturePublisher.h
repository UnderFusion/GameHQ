#pragma once

#include <QString>

// Publishing a capture has to be all-or-nothing. Two things used to break that:
// the name was chosen with an exists-then-use test, so two encoder threads
// finishing in the same second could pick the same path, and the encoder wrote
// straight to that final path, so a crash, a full disk or simply being slow
// left a truncated image where the scanner would happily index it.
//
// A capture is therefore encoded into a "<final>.part" file this service
// exclusively owns, and only a rename makes it appear under its real name. The
// scanner ignores .part, so an unfinished capture is invisible until it is
// whole.
namespace CapturePublisher
{
// Extension of the file a capture is encoded into before it is published.
// Deliberately not one the capture scanner recognises.
extern const QString kPendingSuffix;

struct Reservation
{
    QString finalPath;     // where the finished capture will appear
    QString pendingPath;   // finalPath + kPendingSuffix, created and owned by us

    bool isValid() const { return !pendingPath.isEmpty(); }
};

// Reserves dir/<stamp><suffix>, appending _2, _3, ... on collision. The
// reservation is taken by creating the .part file exclusively, so two threads
// can never be handed the same name.
Reservation reserve(const QString& dir, const QString& stampFormat, const QString& suffix);

// Renames the finished .part onto its final name. On failure the .part stays
// where it is and nothing appears under the final name.
bool publish(const Reservation& reservation, QString* error = nullptr);

// Gives a reservation back after a failed encode.
void discard(const Reservation& reservation);

// Name of a file that belongs to an already reserved capture — a clip's
// thumbnail. Built from the reserved name, so the companion inherits the
// _2, _3 the reservation had to add and can never land on another capture's.
QString companionPath(const QString& finalPath, const QString& dir, const QString& suffix);

// Removes .part files under `root` older than `maxAgeSecs` — what a crash or a
// power cut leaves behind. Returns how many were removed.
int sweepStale(const QString& root, qint64 maxAgeSecs);
}
