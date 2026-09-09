#include "sound/SoundLevels.h"

#include <QAudio>

namespace
{
// Only the events raised by the capture pipeline itself. Kept as an explicit
// list rather than a name prefix so adding an event is a deliberate decision
// about which slider it belongs to.
bool isCaptureEvent(const QString& event)
{
    return event == QLatin1String("screenshot")
        || event == QLatin1String("replay_saved")
        || event == QLatin1String("capture_accepted");
}
}

namespace SoundLevels
{
Level levelFor(const QString& event)
{
    return isCaptureEvent(event) ? Level::Capture : Level::Interface;
}

QString previewEvent()
{
    return QStringLiteral("screenshot");
}

qreal perceptualAmplitude(int percent)
{
    if (percent > 100)
        return qBound(100, percent, 200) / 100.0;
    const qreal slider = qBound(0, percent, 100) / 100.0;
    // Qt's own slider-to-device conversion: the value the user sets is on the
    // logarithmic (perceptual) scale, the device wants linear amplitude.
    return qBound(0.0, qreal(QtAudio::convertVolume(float(slider),
                                                    QtAudio::LogarithmicVolumeScale,
                                                    QtAudio::LinearVolumeScale)),
                  1.0);
}

qreal linearAmplitude(int percent)
{
    return qBound(0, percent, 200) / 100.0;
}
}
