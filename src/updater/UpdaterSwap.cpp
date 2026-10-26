#include "updater/UpdaterSwap.h"
#include "core/DurableFile.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <windows.h>

namespace
{
constexpr std::array<const wchar_t *, 7> kOwned = {
    L"GameHQ.exe", L"app", L"README.txt", L"LICENSE.txt",
    L"THIRD_PARTY_NOTICES.md", L"licenses", L"GameHQUpdater.pending.exe"
};
constexpr std::array<DWORD, 6> kBackoffMs = {100, 250, 500, 1000, 1500, 2000};

bool retryable(DWORD error)
{
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION
        || error == ERROR_ACCESS_DENIED;
}

bool moveWithRetry(const std::filesystem::path &from, const std::filesystem::path &to,
                   std::string &error)
{
    for (std::size_t attempt = 0;; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
            return true;
        const DWORD code = GetLastError();
        if (!retryable(code) || attempt >= kBackoffMs.size()) {
            error = "program-file move failed with Windows error " + std::to_string(code);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kBackoffMs[attempt]));
    }
}

std::filesystem::path journalPath(const updater::Transaction &tx)
{
    return tx.healthTokenPath.parent_path() / L"swap.manifest";
}

bool writeJournal(const updater::Transaction &tx, std::string &error)
{
    const auto journal = journalPath(tx);
    std::filesystem::path partial = journal;
    partial += L".partial";
    std::error_code ec;
    std::filesystem::create_directories(journal.parent_path(), ec);
    if (ec) {
        error = "could not create the program swap journal";
        return false;
    }
    // A presence bit that could not be determined must not be recorded as
    // "absent": rollback would then skip restoring that path.
    std::ostringstream output;
    output << "GAMEHQ_PROGRAM_SWAP_V1\n";
    for (const wchar_t *name : kOwned) {
        const bool oldPresent = std::filesystem::exists(tx.packageRoot / name, ec);
        if (ec) break;
        const bool newPresent = std::filesystem::exists(tx.stagingDir / name, ec);
        if (ec) break;
        output << (oldPresent ? '1' : '0') << ' ' << (newPresent ? '1' : '0') << ' '
               << std::filesystem::path(name).string() << '\n';
    }
    if (ec) {
        error = "could not inspect program files for the swap journal";
        return false;
    }
    switch (durable_file::replace(journal, partial, output.str())) {
    case durable_file::WriteResult::Written:
        return true;
    case durable_file::WriteResult::CreateFailed:
        error = "could not create the program swap journal";
        return false;
    case durable_file::WriteResult::WriteFailed:
        error = "could not finish the program swap journal";
        return false;
    case durable_file::WriteResult::PublishFailed:
        break;
    }
    error = "could not publish the program swap journal";
    return false;
}

struct JournalEntry
{
    std::filesystem::path relative;
    bool oldPresent = false;
    bool newPresent = false;
};

bool readJournal(const updater::Transaction &tx, std::vector<JournalEntry> &entries,
                 std::string &error)
{
    std::ifstream input(journalPath(tx), std::ios::binary);
    std::string line;
    if (!std::getline(input, line) || line != "GAMEHQ_PROGRAM_SWAP_V1") {
        error = "program swap journal is missing or invalid";
        return false;
    }
    for (const wchar_t *name : kOwned) {
        if (!std::getline(input, line)) {
            error = "program swap journal is incomplete";
            return false;
        }
        const std::string expected = std::filesystem::path(name).string();
        if (line.size() < 5 || line[1] != ' ' || line[3] != ' '
            || (line[0] != '0' && line[0] != '1')
            || (line[2] != '0' && line[2] != '1') || line.substr(4) != expected) {
            error = "program swap journal contains an invalid entry";
            return false;
        }
        entries.push_back({std::filesystem::path(name), line[0] == '1', line[2] == '1'});
    }
    if (std::getline(input, line)) {
        error = "program swap journal contains unexpected entries";
        return false;
    }
    return true;
}
} // namespace

namespace updater
{
bool swapProgramFiles(const Transaction &tx, std::string &error)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(tx.stagingDir) || std::filesystem::exists(tx.backupDir)) {
        error = "validated staging is missing or backup already exists";
        return false;
    }
    if (!writeJournal(tx, error))
        return false;
    if (!std::filesystem::create_directories(tx.backupDir, ec) || ec) {
        std::filesystem::remove(journalPath(tx), ec);
        error = "could not create program backup directory";
        return false;
    }
    struct Step { std::filesystem::path relative; bool oldMoved = false; bool newMoved = false; };
    std::vector<Step> steps;
    const auto rollback = [&] {
        bool restored = true;
        std::string rollbackError;
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            const auto live = tx.packageRoot / it->relative;
            const auto staged = tx.stagingDir / it->relative;
            const auto backup = tx.backupDir / it->relative;
            if (it->newMoved) {
                std::filesystem::create_directories(staged.parent_path(), ec);
                if (!moveWithRetry(live, staged, rollbackError)) restored = false;
            }
            if (it->oldMoved) {
                std::filesystem::create_directories(live.parent_path(), ec);
                if (!moveWithRetry(backup, live, rollbackError)) restored = false;
            }
        }
        if (restored) {
            std::filesystem::remove_all(tx.backupDir, ec);
            std::filesystem::remove(journalPath(tx), ec);
        }
        return restored;
    };
    for (const wchar_t *name : kOwned) {
        Step step{std::filesystem::path(name)};
        const auto live = tx.packageRoot / step.relative;
        const auto staged = tx.stagingDir / step.relative;
        const auto backup = tx.backupDir / step.relative;
        if (!std::filesystem::exists(live) && !std::filesystem::exists(staged))
            continue;
        steps.push_back(step);
        Step &active = steps.back();
        if (std::filesystem::exists(live)) {
            std::filesystem::create_directories(backup.parent_path(), ec);
            if (ec || !moveWithRetry(live, backup, error)) {
                if (!rollback()) error += "; rollback incomplete, backup retained";
                return false;
            }
            active.oldMoved = true;
        }
        if (std::filesystem::exists(staged)) {
            std::filesystem::create_directories(live.parent_path(), ec);
            if (ec || !moveWithRetry(staged, live, error)) {
                if (!rollback()) error += "; rollback incomplete, backup retained";
                return false;
            }
            active.newMoved = true;
        }
    }
    return true;
}

bool hasProgramSwapJournal(const Transaction &tx)
{
    return std::filesystem::is_regular_file(journalPath(tx));
}

bool rollbackProgramFiles(const Transaction &tx, std::string &error)
{
    std::vector<JournalEntry> entries;
    if (!readJournal(tx, entries, error))
        return false;
    const auto failedDir = tx.healthTokenPath.parent_path() / L"failed-new";
    std::error_code ec;
    std::filesystem::remove_all(failedDir, ec);
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        const auto live = tx.packageRoot / it->relative;
        const auto staged = tx.stagingDir / it->relative;
        const auto backup = tx.backupDir / it->relative;
        if (it->oldPresent && std::filesystem::exists(backup)) {
            if (std::filesystem::exists(live)) {
                const auto failed = failedDir / it->relative;
                std::filesystem::create_directories(failed.parent_path(), ec);
                if (ec || !moveWithRetry(live, failed, error))
                    return false;
            }
            std::filesystem::create_directories(live.parent_path(), ec);
            if (ec || !moveWithRetry(backup, live, error))
                return false;
        } else if (!it->oldPresent && it->newPresent && std::filesystem::exists(live)) {
            if (std::filesystem::exists(staged)) {
                error = "rollback found both live and staged copies of a newly added path";
                return false;
            }
            std::filesystem::create_directories(staged.parent_path(), ec);
            if (ec || !moveWithRetry(live, staged, error))
                return false;
        }
    }
    std::filesystem::remove_all(tx.backupDir, ec);
    std::filesystem::remove_all(failedDir, ec);
    std::filesystem::remove(journalPath(tx), ec);
    std::filesystem::remove(tx.healthTokenPath, ec);
    return true;
}
} // namespace updater
