#include "updater/UpdaterTransaction.h"
#include "updater/UpdaterStaging.h"
#include "updater/UpdaterDataSnapshot.h"
#include "updater/UpdaterSwap.h"
#include "updater/UpdaterHealth.h"
#include "updater/UpdaterRecovery.h"
#include "core/UpdaterHandshake.h"
#include "core/DllSearchPolicy.h"
#include "core/UpdateMaintenance.h"
#include "security/ReleaseTrust.h"
#include "security/ReleaseManifest.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <windows.h>

namespace
{
struct HandleCloser { void operator()(void *handle) const { if (handle) CloseHandle(handle); } };
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// The helper usually runs detached, so cout/cerr are invisible; mirror every
// outcome into .update/updater.log. Never opened for --dry-run, which
// promises not to write anything.
std::ofstream g_log;

void openLog(const std::filesystem::path &transactionPath)
{
    g_log.open(transactionPath.parent_path() / L"updater.log",
               std::ios::app | std::ios::binary);
}

std::string timestamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

void note(const std::string &line)
{
    std::cout << line << '\n';
    if (g_log) {
        g_log << timestamp() << ' ' << line << '\n';
        g_log.flush();
    }
}

int failWith(const std::string &error, int code)
{
    std::cerr << "ERROR: " << error << '\n';
    if (g_log) {
        g_log << timestamp() << " ERROR: " << error << " (exit " << code << ")\n";
        g_log.flush();
    }
    return code;
}

std::wstring mutexNameFor(const std::filesystem::path &transactionPath)
{
    std::wstring normalized = std::filesystem::absolute(transactionPath).lexically_normal().wstring();
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), towlower);
    std::uint64_t hash = 1469598103934665603ULL;
    for (wchar_t ch : normalized) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return L"Local\\GameHQUpdater-" + std::to_wstring(hash);
}

int runTransaction(const std::filesystem::path &transactionPath, const std::wstring &mode)
{
    if (mode != L"--dry-run")
        openLog(transactionPath);
    UniqueHandle activeMutex(CreateMutexW(nullptr, TRUE, L"Local\\GameHQUpdaterActive"));
    if (!activeMutex || GetLastError() == ERROR_ALREADY_EXISTS)
        return failWith("another updater helper is already active", 4);
    const std::wstring mutexName = mutexNameFor(transactionPath);
    UniqueHandle mutex(CreateMutexW(nullptr, TRUE, mutexName.c_str()));
    if (!mutex)
        return failWith("could not create updater transaction mutex", 3);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return failWith("another updater is already processing this transaction", 4);

    updater::Transaction transaction;
    std::string error;
    if (!updater::loadAndValidateTransaction(transactionPath, transaction, error))
        return failWith(error, 2);
    // Schema 1 identified the caller by process id alone. Recovery only
    // restores what is already on disk and needs no caller identity, but every
    // mode that installs or moves files does.
    if (transaction.schemaVersion < 2 && mode != L"--dry-run" && mode != L"--recover")
        return failWith("this update was prepared by an older GameHQ; run the update again", 2);
    if (mode != L"--dry-run")
        note("GameHQUpdater " GAMEHQ_UPDATER_VERSION " starting mode "
             + std::string(mode.begin(), mode.end()) + " for version "
             + transaction.expectedVersion);
    if (mode == L"--dry-run") {
        std::cout << "DRY RUN - no files will be changed\n";
        std::cout << "TRANSACTION version=" << transaction.expectedVersion
                  << " phase=" << transaction.phase << '\n';
        int index = 1;
        for (const std::string &operation : updater::plannedOperations(transaction))
            std::cout << "PLAN " << index++ << ": " << operation << '\n';
        return 0;
    }
    if (mode == L"--snapshot-data") {
        if (!updater::createDataSnapshot(transaction.dataDir, transaction.dataSnapshotDir, error))
            return failWith(error, 6);
        note("DATA SNAPSHOT READY");
        return 0;
    }
    if (mode == L"--restore-data") {
        if (!updater::restoreDataSnapshot(transaction.dataDir, transaction.dataSnapshotDir, error))
            return failWith(error, 7);
        note("DATA SNAPSHOT RESTORED");
        return 0;
    }
    if (mode == L"--swap") {
        if (!updater::swapProgramFiles(transaction, error))
            return failWith(error, 8);
        note("PROGRAM FILES SWAPPED");
        return 0;
    }
    if (mode == L"--wait-health") {
        if (!updater::launchAndWaitForHealth(transaction, updater::kHealthTimeoutMs, error))
            return failWith(error, 9);
        note("UPDATED APPLICATION HEALTHY");
        return 0;
    }
    if (mode == L"--apply") {
        // Handshake with the caller (docs/updater.md). Order matters: the exact
        // authorising process is opened and its identity confirmed BEFORE READY
        // is signalled, so the app only exits once the helper is already
        // holding a handle it can meaningfully wait on. Nothing has been
        // mutated yet, so every failure below aborts without needing recovery
        // and must release maintenance: leaving the marker behind would block
        // Setup and the next launch over a failure that changed no file.
        const auto failHandoff = [&](const std::string &reason) {
            maintenance::finish(transaction.packageRoot);
            return failWith(reason + " (no files were changed)", 12);
        };

        bool callerAlreadyExited = false;
        UniqueHandle caller(updater::openAuthorisingCaller(transaction, callerAlreadyExited, error));
        if (!caller && !callerAlreadyExited)
            return failHandoff(error);

        UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE,
                                        handshake::readyEventNameFor(transactionPath).c_str()));
        if (!ready)
            return failHandoff("could not open the updater handshake event");
        if (!SetEvent(ready.get()))
            return failHandoff("could not signal the updater handshake event");

        if (caller) {
            // Only a clean, observed exit permits mutation. A timeout, an
            // abandoned wait or a failed wait all leave the old process
            // possibly alive and holding files.
            const DWORD waited = WaitForSingleObject(caller.get(), 60000);
            if (waited != WAIT_OBJECT_0) {
                return failHandoff(waited == WAIT_TIMEOUT
                    ? "the running application did not exit before the update"
                    : waited == WAIT_ABANDONED
                        ? "the wait for the running application was abandoned"
                        : "the wait for the running application failed");
            }
        }
        if (!updater::applyUpdate(transaction, updater::kHealthTimeoutMs, error))
            return failWith(error, 10);
        note("UPDATE APPLIED AND HEALTHY");
        return 0;
    }
    if (mode == L"--recover") {
        if (!updater::recoverInterruptedUpdate(transaction, error))
            return failWith(error, 11);
        note("UPDATE RECOVERY COMPLETE");
        return 0;
    }
    if (!updater::extractAndValidatePackage(transaction, error))
        return failWith(error, 5);
    note("STAGED AND VALIDATED version=" + transaction.expectedVersion);
    return 0;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    dll_search::restrictSearchPath();
    const std::wstring mode = argc >= 2 ? std::wstring(argv[1]) : std::wstring();
    if (argc == 2 && mode == L"--self-test") {
        std::cout << "GameHQUpdater " GAMEHQ_UPDATER_VERSION " ready\n";
        return 0;
    }
    if (argc == 2 && mode == L"--release-trust-self-test") {
        std::string error;
        if (!release_trust::runBuiltInSelfTest(error)) {
            std::cerr << "ERROR: release trust self-test failed: " << error << '\n';
            return 13;
        }
        // Report the compiled trust table so release evidence records exactly
        // which keys this binary would accept (t48 flips this to production).
        const auto &keys = release_manifest::trustedKeys();
        if (keys.empty()) {
            std::cerr << "ERROR: this build trusts no release signing key\n";
            return 13;
        }
        for (const auto &key : keys)
            std::cout << "TRUSTED KEY " << key.keyId << '\n';
        std::cout << "TRUST TABLE "
                  << (release_manifest::trustTableIsTestOnly() ? "test-only" : "production") << '\n';
        std::cout << "GameHQUpdater release trust self-test passed\n";
        return 0;
    }
    if (argc != 3 || (mode != L"--dry-run" && mode != L"--stage"
                      && mode != L"--snapshot-data" && mode != L"--restore-data"
                      && mode != L"--swap" && mode != L"--wait-health"
                      && mode != L"--apply" && mode != L"--recover")) {
        std::cerr << "Usage: GameHQUpdater.exe <mode> <transaction.json>\n";
        return 1;
    }
    return runTransaction(std::filesystem::path(argv[2]), mode);
}
