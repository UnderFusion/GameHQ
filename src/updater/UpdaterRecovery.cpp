#include "updater/UpdaterRecovery.h"
#include "core/DurableFile.h"
#include "core/UpdateMaintenance.h"
#include "updater/UpdaterDataSnapshot.h"
#include "updater/UpdaterHealth.h"
#include "updater/UpdaterStaging.h"
#include "updater/UpdaterSwap.h"

#include <fstream>
#include <set>
#include <vector>
#include <windows.h>

namespace
{
std::filesystem::path phasePath(const updater::Transaction &tx)
{
    return tx.healthTokenPath.parent_path() / L"transaction.phase";
}

std::string readPhase(const updater::Transaction &tx)
{
    std::ifstream input(phasePath(tx), std::ios::binary);
    std::string phase;
    std::getline(input, phase);
    if (!phase.empty() && phase.back() == '\r') phase.pop_back();
    return phase;
}

std::wstring quoteArgument(const std::wstring &value)
{
    std::wstring quoted = L"\"";
    unsigned backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') ++backslashes;
        else if (ch == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

// The updated application writes this token only after it has run healthy for
// several seconds, and the helper deletes any stale copy before launching it.
// A matching token is therefore proof the update itself succeeded.
bool healthTokenConfirms(const updater::Transaction &tx)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(tx.healthTokenPath, ec);
    if (ec || size == 0 || size > 64)
        return false;
    std::ifstream input(tx.healthTokenPath, std::ios::binary);
    std::string token;
    std::getline(input, token);
    if (!token.empty() && token.back() == '\r')
        token.pop_back();
    return !token.empty() && token == tx.expectedVersion;
}

bool launchPrevious(const updater::Transaction &tx, std::string &error)
{
    std::wstring command = quoteArgument(tx.restartExecutable.wstring());
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tx.restartExecutable.c_str(), mutableCommand.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, tx.packageRoot.c_str(), &startup, &process)) {
        error = "recovery completed but the application could not be restarted (Windows error "
            + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool rollbackAll(const updater::Transaction &tx, std::string &error)
{
    std::string dataError;
    std::string programError;
    bool dataOk = true;
    if (std::filesystem::is_directory(tx.dataSnapshotDir))
        dataOk = updater::restoreDataSnapshot(tx.dataDir, tx.dataSnapshotDir, dataError);
    bool programOk = true;
    if (updater::hasProgramSwapJournal(tx))
        programOk = updater::rollbackProgramFiles(tx, programError);
    if (!dataOk || !programOk) {
        error = (!dataOk ? dataError : std::string())
            + (!dataOk && !programOk ? "; " : std::string())
            + (!programOk ? programError : std::string());
        return false;
    }
    return true;
}
}

namespace updater
{
bool writeTransactionPhase(const Transaction &tx, const std::string &phase,
                           std::string &error)
{
    static const std::set<std::string> allowed = {
        "staged", "data_snapshotted", "swapped", "validating", "healthy", "rolled_back"
    };
    if (!allowed.contains(phase)) {
        error = "refusing to write an unknown update phase";
        return false;
    }
    const auto path = phasePath(tx);
    std::filesystem::path partial = path;
    partial += L".partial";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "could not create the update phase marker";
        return false;
    }
    switch (durable_file::replace(path, partial, phase + '\n')) {
    case durable_file::WriteResult::Written:
        return true;
    case durable_file::WriteResult::CreateFailed:
        error = "could not create the update phase marker";
        return false;
    case durable_file::WriteResult::WriteFailed:
        error = "could not finish the update phase marker";
        return false;
    case durable_file::WriteResult::PublishFailed:
        break;
    }
    error = "could not publish the update phase marker";
    return false;
}

bool applyUpdate(const Transaction &tx, int healthTimeoutMs, std::string &error)
{
    const std::string existing = readPhase(tx);
    if (hasProgramSwapJournal(tx) || std::filesystem::is_directory(tx.dataSnapshotDir)
        || (!existing.empty() && existing != "rolled_back")) {
        error = "an unfinished update requires recovery before another apply";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(phasePath(tx), ec);
    // Recovery treats a matching token as proof of health, so none may survive
    // from an earlier attempt.
    std::filesystem::remove(tx.healthTokenPath, ec);
    if (ec) {
        error = "could not remove a stale health token";
        return false;
    }
    // With no journal/snapshot there is no live mutation to recover. A staging
    // directory is therefore only an abandoned extraction and is safe to purge.
    std::filesystem::remove_all(tx.stagingDir, ec);
    auto failAndRollback = [&](const std::string &cause) {
        std::string rollbackError;
        if (!rollbackAll(tx, rollbackError)) {
            error = cause + "; rollback incomplete: " + rollbackError;
            return false;
        }
        std::error_code cleanupError;
        std::filesystem::remove_all(tx.stagingDir, cleanupError);
        std::filesystem::remove_all(tx.dataSnapshotDir, cleanupError);
        if (!writeTransactionPhase(tx, "rolled_back", rollbackError)) {
            error = cause + "; rollback completed but its phase could not be recorded: " + rollbackError;
            return false;
        }
        maintenance::finish(tx.packageRoot);
        std::string launchError;
        if (!launchPrevious(tx, launchError)) {
            error = cause + "; " + launchError;
            return false;
        }
        error = cause + "; the previous version was restored and restarted";
        return false;
    };

    if (!extractAndValidatePackage(tx, error))
        return failAndRollback(error);
    if (!writeTransactionPhase(tx, "staged", error))
        return failAndRollback(error);
    if (!createDataSnapshot(tx.dataDir, tx.dataSnapshotDir, error))
        return failAndRollback(error);
    if (!writeTransactionPhase(tx, "data_snapshotted", error))
        return failAndRollback(error);
    if (!swapProgramFiles(tx, error))
        return failAndRollback(error);
    if (!writeTransactionPhase(tx, "swapped", error)
        || !writeTransactionPhase(tx, "validating", error))
        return failAndRollback(error);
    if (!launchAndWaitForHealth(tx, healthTimeoutMs, error))
        return failAndRollback(error);
    // From here the updated application is running and has proven itself
    // healthy. Rolling back now would move program files and restore data
    // underneath that live process and start a second copy, so a phase that
    // cannot be recorded is retried and then reported, never rolled back.
    // Recovery accepts the published health token for exactly this case.
    bool recorded = false;
    for (const DWORD delayMs : {0, 250, 1000, 2000}) {
        Sleep(delayMs);
        if ((recorded = writeTransactionPhase(tx, "healthy", error)))
            break;
    }
    maintenance::finish(tx.packageRoot);
    if (!recorded) {
        error = "the updated application is healthy but its phase could not be recorded: " + error;
        return false;
    }
    return true;
}

bool recoverInterruptedUpdate(const Transaction &tx, std::string &error)
{
    const std::string phase = readPhase(tx);
    if (phase == "healthy" || phase == "rolled_back")
        return true;
    if (phase.empty() && !hasProgramSwapJournal(tx)
        && !std::filesystem::is_directory(tx.dataSnapshotDir))
        return true;
    if (phase == "validating" && healthTokenConfirms(tx)) {
        // The updated application reported health but the helper never
        // recorded it (crash, power loss or a failed phase write). Keep the
        // update; the second healthy start cleans the rollback material.
        if (!writeTransactionPhase(tx, "healthy", error))
            return false;
        maintenance::finish(tx.packageRoot);
        return launchPrevious(tx, error);
    }
    if (!rollbackAll(tx, error))
        return false;
    std::error_code ec;
    std::filesystem::remove_all(tx.stagingDir, ec);
    std::filesystem::remove_all(tx.dataSnapshotDir, ec);
    if (!writeTransactionPhase(tx, "rolled_back", error))
        return false;
    maintenance::finish(tx.packageRoot);
    return launchPrevious(tx, error);
}
}
