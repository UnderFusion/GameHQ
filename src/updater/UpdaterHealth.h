#pragma once
#include "updater/UpdaterTransaction.h"
#include <string>

namespace updater
{
// Upper bound for the updated application's first start. The app publishes its
// health token about seven seconds after a full init, and a first start after
// an update can be slow (antivirus scanning every new binary, database
// migrations, a cold disk). The wait ends as soon as the token appears or the
// whole new process tree has exited, so the bound only matters for an app that
// is alive but never becomes healthy.
constexpr int kHealthTimeoutMs = 120000;

bool launchAndWaitForHealth(const Transaction &transaction, int timeoutMs,
                            std::string &errorOut);
}
