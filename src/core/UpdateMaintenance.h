#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace maintenance
{
enum class State { Inactive, Active, StaleRecovery };

struct Info
{
    State state = State::Inactive;
    std::string phase;
};

bool begin(const std::filesystem::path &packageRoot, std::string &error);
void finish(const std::filesystem::path &packageRoot);
// Completed transactions deliberately retain their journal until a later
// successful application start. That journal alone must not trigger recovery.
bool recoveryRequired(const std::filesystem::path &packageRoot);
// Call only after a subsequent successful start. A failed cleanup keeps the
// terminal phase until every journal/data artifact has been retired.
bool cleanupCompleted(const std::filesystem::path &packageRoot, std::string &error);
Info inspect(const std::filesystem::path &packageRoot);
Info inspect(const std::filesystem::path &packageRoot, bool helperActive,
             std::filesystem::file_time_type now,
             std::chrono::seconds staleAfter = std::chrono::minutes(5));
} // namespace maintenance
