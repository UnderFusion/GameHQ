#pragma once
#include <QByteArray>
#ifndef GAMEHQ_SONY_HID_READER_DEFAULT
#define GAMEHQ_SONY_HID_READER_DEFAULT 0
#endif
static_assert(GAMEHQ_SONY_HID_READER_DEFAULT == 0 || GAMEHQ_SONY_HID_READER_DEFAULT == 1);
namespace SonyHidReaderPolicy {
struct Decision { bool enabled; const char* reason; };
inline Decision resolve(const QByteArray& overrideValue,
                        bool buildDefault = GAMEHQ_SONY_HID_READER_DEFAULT != 0)
{
    if (overrideValue == "0") return {false, "environment override"};
    if (overrideValue == "1") return {true, "environment override"};
    return {buildDefault, buildDefault ? "beta build default" : "normal build default"};
}
} // namespace SonyHidReaderPolicy
