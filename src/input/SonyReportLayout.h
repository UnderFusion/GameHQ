#pragma once

namespace SonyReportLayout
{
enum class Family {
    DualSense,
    Ds4,
};

// Byte offsets of the left-stick axes (LX LY RX RY) and the button block
// (hat+face, shoulders/menu, PS) inside one raw HID input report, report id
// included at byte 0. -1 means the report is not a state report we parse.
struct Offsets {
    int axes = -1;
    int buttons = -1;
    const char* variant = "unsupported";   // for the layout log line only
    bool valid() const { return buttons >= 0; }
};

// A Bluetooth DualSense simple report 0x01 is 10 bytes, but Raw Input can
// deliver it as 78 (the longest input report of that HID collection). Only
// USB sends 0x01 at 64 bytes. SDL's hidapi PS5 driver uses the same 10/78
// rule.
inline bool isDualSenseSimpleReport(int len)
{
    return len == 10 || len == 78;
}

// Layouts, matching SDL_hidapi_ps5/ps4.c and Linux hid-playstation.c:
//   DS4  USB/BT-simple 0x01: [id] LX LY RX RY buttons...          -> axes 1, buttons 5
//   DS4  BT          0x11: [id] 2 bytes transport, then as USB   -> axes 3, buttons 7
//   DS   USB         0x01: [id] LX LY RX RY L2 R2 counter buttons -> axes 1, buttons 8
//   DS   BT-simple   0x01: [id] LX LY RX RY buttons... L2 R2      -> axes 1, buttons 5
//   DS   BT          0x31: [id] 1 byte seq tag, then as USB      -> axes 2, buttons 9
// Offsets count from byte 0 = report id. DualSense 0x31 has ONE extra byte
// after the id, so its data starts at index 2 (DS4 0x11 has two, index 3).
// Reading 0x31 from index 3 shifted every field: stick up/down read as
// left/right.
inline Offsets locate(unsigned char reportId, Family family, int len)
{
    const bool ds4 = family == Family::Ds4;
    if (reportId == 0x01) {
        if (ds4)
            return {1, 5, "ds4-usb-or-bt-simple"};
        if (isDualSenseSimpleReport(len))
            return {1, 5, "dualsense-bt-simple"};
        return {1, 8, "dualsense-usb"};
    }
    if (reportId == 0x11 && ds4)
        return {3, 7, "ds4-bt-full"};
    if (reportId == 0x31 && !ds4)
        return {2, 9, "dualsense-bt-full"};
    return {};
}
} // namespace SonyReportLayout
