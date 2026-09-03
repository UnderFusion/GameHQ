#pragma once

#define IDS_LAUNCHER_TITLE 100
#define IDS_LAUNCHER_PATH_TOO_DEEP 101
#define IDS_LAUNCHER_UPDATE_ACTIVE 102
#define IDS_LAUNCHER_EXE_MISSING 103
#define IDS_LAUNCHER_COMMAND_TOO_LONG 104
#define IDS_LAUNCHER_START_FAILED 105

// English-only test seam proving that a missing localized entry resolves
// through the complete en-US table rather than leaking an empty string.
#define IDS_LAUNCHER_ENGLISH_FALLBACK_PROBE 106
