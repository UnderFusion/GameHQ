#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>

// One user-visible capture request, from the press that started it to the
// outcome the user sees (docs/capture-engine.md).
//
// Before this existed, a save that failed early emitted clipFailed() and
// nothing else: the log showed no trace of the press at all, so "I pressed the
// button and nothing happened" could not be told apart from "the button never
// reached GameHQ". The id is minted at the input edge and used as the chain id
// in every stage line, so one press is one greppable chain.
struct CaptureRequest
{
    // Where the press came from. Recorded once, on the accepted line, because
    // the same action reaches the same service from four different surfaces.
    enum class Source { Unknown, Controller, Keyboard, Mouse, Overlay, Ui, Tray };

    quint64 id = 0;
    Source source = Source::Unknown;
    // Milliseconds since the first request of this run. Relative on purpose:
    // it survives a log paste with the wall clock stripped, and the deltas
    // between stages are what a stall investigation needs.
    qint64 monotonicMs = 0;

    // A fresh request with the next id. Ids are unique and increasing for the
    // life of the process; 0 is reserved for "no request".
    static CaptureRequest create(Source source);

    // The device group BindingRuntime dispatched from ("controller",
    // "keyboard", ...) mapped onto a source. Unrecognized groups stay Unknown
    // rather than guessing.
    static Source sourceForDeviceGroup(const QString& deviceGroup);

    static QString label(Source source);

    bool isValid() const { return id != 0; }

    // "12 src=controller +4231ms" — the chain id first, so a log line reads
    // ReplaySave[12 src=controller +4231ms] and grepping "[12 " is enough.
    QString tag() const;
};

Q_DECLARE_METATYPE(CaptureRequest)
