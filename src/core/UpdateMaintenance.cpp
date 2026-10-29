#include "core/UpdateMaintenance.h"
#include "core/DurableFile.h"

#include <fstream>
#include <windows.h>

namespace
{
std::filesystem::path markerPath(const std::filesystem::path &root)
{
    return root / L".update" / L"maintenance.lock";
}

std::string readPhase(const std::filesystem::path &root)
{
    std::ifstream input(root / L".update" / L"transaction.phase", std::ios::binary);
    std::string phase;
    std::getline(input, phase);
    if (!phase.empty() && phase.back() == '\r')
        phase.pop_back();
    return phase;
}

bool updaterHelperActive()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\GameHQUpdaterActive");
    if (!mutex)
        return false;
    CloseHandle(mutex);
    return true;
}

bool completedPhase(const std::string &phase)
{
    return phase == "healthy" || phase == "rolled_back";
}
}

namespace maintenance
{
bool begin(const std::filesystem::path &packageRoot, std::string &error)
{
    error.clear();
    const std::filesystem::path marker = markerPath(packageRoot);
    std::filesystem::path partial = marker;
    partial += L".partial";
    std::error_code ec;
    std::filesystem::create_directories(marker.parent_path(), ec);
    const durable_file::WriteResult written = ec
        ? durable_file::WriteResult::CreateFailed
        : durable_file::replace(marker, partial, "update\n");
    if (written == durable_file::WriteResult::CreateFailed) {
        error = "could not create the update maintenance marker";
        return false;
    }
    if (written != durable_file::WriteResult::Written) {
        error = "could not publish the update maintenance marker";
        return false;
    }
    return true;
}

void finish(const std::filesystem::path &packageRoot)
{
    std::error_code ec;
    std::filesystem::remove(markerPath(packageRoot), ec);
}

bool recoveryRequired(const std::filesystem::path &packageRoot)
{
    const std::string phase = readPhase(packageRoot);
    if (completedPhase(phase))
        return false;
    std::error_code ec;
    return !phase.empty()
        || std::filesystem::exists(packageRoot / L".update" / L"swap.manifest", ec);
}

bool cleanupCompleted(const std::filesystem::path &packageRoot, std::string &error)
{
    error.clear();
    if (!completedPhase(readPhase(packageRoot))) {
        error = "update cleanup requires a completed transaction";
        return false;
    }
    const auto update = packageRoot / L".update";
    // Stop on the first failure. In particular, never erase the terminal phase
    // while a journal still exists: that would turn a cleanup failure into a
    // spurious interrupted update on the next launch.
    for (const wchar_t *name : {L"backup", L"data-snapshot", L"staging",
                                 L"failed-new", L"downloads"}) {
        std::error_code ec;
        std::filesystem::remove_all(update / name, ec);
        if (ec) {
            error = "could not clean completed update directory: "
                + std::filesystem::path(name).string() + " (" + ec.message() + ")";
            return false;
        }
    }
    for (const wchar_t *name : {L"swap.manifest", L"healthy.token",
                                 L"transaction.json", L"transaction.phase"}) {
        std::error_code ec;
        std::filesystem::remove(update / name, ec);
        if (ec) {
            error = "could not clean completed update marker: "
                + std::filesystem::path(name).string() + " (" + ec.message() + ")";
            return false;
        }
    }
    return true;
}

Info inspect(const std::filesystem::path &packageRoot, bool helperActive,
             std::filesystem::file_time_type now, std::chrono::seconds staleAfter)
{
    const std::filesystem::path marker = markerPath(packageRoot);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(marker, ec))
        return {};

    const std::string phase = readPhase(packageRoot);
    if (helperActive)
        return { State::Active, phase };
    if (completedPhase(phase))
        return { State::Inactive, phase };

    const auto modified = std::filesystem::last_write_time(marker, ec);
    if (ec)
        return { State::Active, phase };
    if (now > modified && now - modified > staleAfter)
        return { State::StaleRecovery, phase };
    return { State::Active, phase };
}

Info inspect(const std::filesystem::path &packageRoot)
{
    return inspect(packageRoot, updaterHelperActive(),
                   std::filesystem::file_time_type::clock::now());
}
} // namespace maintenance
