#pragma once

#include <windows.h>

namespace dll_search
{
// Call first thing in every entry point. Later LoadLibrary calls (ours, Qt's,
// and any system component's) then search only the application directory,
// System32 and explicitly added directories - never the current working
// directory or PATH. Attacker model: someone who can write a DLL into the CWD
// (often a downloads or capture folder) or into a PATH directory must not get
// it loaded into GameHQ. The application directory stays searchable on
// purpose: Qt, its plugins, FFmpeg, GameInput, TDLib and the MinGW runtime are
// shipped there, and whoever can write there can already replace the exe.
inline void restrictSearchPath()
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
} // namespace dll_search
