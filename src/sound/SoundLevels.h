#pragma once
#include <QString>
#include <QtGlobal>

// Which volume a UI sound answers to, and how a slider percentage becomes an
// amplitude. Kept apart from SoundEngine so the rules can be tested without an
// audio device, in the same spirit as SoundLoadTracker.
//
// Capture feedback is heard over a running game and judged against the game's
// own audio, so it carries its own level. Navigation and dialog sounds are
// heard in a quiet app and keep the interface level they always had.
namespace SoundLevels
{
enum class Level
{
    Interface,   // sounds.volume
    Capture,     // sounds.capture_volume
};

// event names as passed to SoundEngine::play().
//
// "error" stays on the interface level on purpose: the same asset also reports
// settings failures such as a refused portable import, and moving it would put
// an unrelated sound under a capture slider.
Level levelFor(const QString& event);

// The event the capture preview plays. It is the real saved-screenshot sound,
// so the slider is judged with what it actually governs rather than a
// test-only asset.
QString previewEvent();

// Slider percent (0-200) to gain relative to the original asset, read as a
// perceptual scale: half-way sounds about half as loud instead of carrying
// half the amplitude, which is barely a difference to the ear.
qreal perceptualAmplitude(int percent);

// Bundled playback assets contain exactly twice the original PCM amplitude.
// Divide gain by this factor before passing it to QSoundEffect (which caps at 1).
inline constexpr qreal assetGain = 2.0;

// The interface level keeps its original straight percentage mapping. Making
// it perceptual would quietly move the UI sounds of every existing user who
// never touched the slider.
qreal linearAmplitude(int percent);
}
