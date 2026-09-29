#pragma once

#include <QLatin1StringView>

// The one TDLib build GameHQ will load (t22). It is the official tdlib/td
// source at this exact commit, built by tools/tdlib/build-tdjson.ps1 into our
// own tdjson.dll; no third-party prebuilt DLL is ever accepted. Bumping the
// pin means: change these constants, third_party/tdlib/tdlib-pin.json and the
// hash together, rebuild the DLL, rerun tst_tdruntime.
namespace telegram::tdpin
{
inline constexpr QLatin1StringView kRepository{ "https://github.com/tdlib/td" };
inline constexpr QLatin1StringView kVersion{ "1.8.67" };
inline constexpr QLatin1StringView kCommit{ "bc9c263e2bfee06aaab41e82db51a103376030bc" };
// SHA-256 of the tdjson.dll produced by the pinned build, lowercase hex.
// Empty until the first reproducible build is recorded: while empty the
// production loader reports Unpinned and never loads anything.
inline constexpr QLatin1StringView kRuntimeSha256{ "" };
// Where the optional runtime is installed, relative to GameHQ.exe.
inline constexpr QLatin1StringView kRuntimeRelativeDir{ "runtime/tdlib" };
inline constexpr QLatin1StringView kRuntimeFileName{ "tdjson.dll" };
} // namespace telegram::tdpin
