#include "updater/UpdaterHealth.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <windows.h>

namespace
{
std::wstring quoteArgument(const std::wstring &value)
{
    std::wstring quoted = L"\"";
    unsigned backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
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

// Counts every process still running in the supervision job: the launcher
// exits as soon as it has started app/GameHQ.exe, so the process handle alone
// says nothing about whether the application is still starting. A failed query
// is treated as "still running" so it can only lengthen the wait, never cut it.
DWORD activeProcessCount(HANDLE job)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                   &accounting, sizeof(accounting), nullptr))
        return 1;
    return accounting.ActiveProcesses;
}

std::string trimmedToken(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();
    std::string value = contents.str();
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
        value.pop_back();
    return value;
}
}

namespace updater
{
bool launchAndWaitForHealth(const Transaction &transaction, int timeoutMs,
                            std::string &errorOut)
{
    errorOut.clear();
    std::error_code ec;
    std::filesystem::remove(transaction.healthTokenPath, ec);
    if (ec) {
        errorOut = "could not remove a stale health token";
        return false;
    }

    std::wstring command = quoteArgument(transaction.restartExecutable.wstring())
        + L" --post-update "
        + quoteArgument(std::wstring(transaction.expectedVersion.begin(), transaction.expectedVersion.end()))
        + L" " + quoteArgument(transaction.healthTokenPath.wstring());
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(transaction.restartExecutable.c_str(), mutableCommand.data(), nullptr,
                        nullptr, FALSE, CREATE_SUSPENDED, nullptr, transaction.packageRoot.c_str(),
                        &startup, &process)) {
        errorOut = "could not launch the updated application (Windows error "
            + std::to_string(GetLastError()) + ")";
        return false;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                          &limits, sizeof(limits))
        || !AssignProcessToJobObject(job, process.hProcess)
        || ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD code = GetLastError();
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (job) CloseHandle(job);
        errorOut = "could not supervise the updated application process tree (Windows error "
            + std::to_string(code) + ")";
        return false;
    }
    CloseHandle(process.hThread);

    // The deadline is a generous upper bound for a slow first start, not the
    // expected wait: a process tree that has already exited can never publish
    // the token, so it is reported at once instead of waiting out the bound.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    bool treeExited = false;
    while (std::chrono::steady_clock::now() < deadline) {
        // Sampled before the token check, so a process that publishes the
        // token and then exits is still seen as healthy on this pass.
        treeExited = activeProcessCount(job) == 0;
        if (std::filesystem::is_regular_file(transaction.healthTokenPath, ec)) {
            const std::string token = trimmedToken(transaction.healthTokenPath);
            if (token.empty() && !treeExited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }
            if (token == transaction.expectedVersion) {
                limits.BasicLimitInformation.LimitFlags = 0;
                if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                             &limits, sizeof(limits))) {
                    // Kill-on-close could not be lifted, so the healthy app
                    // dies as soon as the job handle closes (at the latest at
                    // helper exit). Report failure so the caller rolls back
                    // and restarts the previous version instead.
                    CloseHandle(job);
                    CloseHandle(process.hProcess);
                    errorOut = "could not release the updated application from supervision";
                    return false;
                }
                CloseHandle(job);
                CloseHandle(process.hProcess);
                return true;
            }
            CloseHandle(job); // kills the invalid new process tree before rollback
            CloseHandle(process.hProcess);
            errorOut = "the updated application wrote an invalid health token";
            return false;
        }
        ec.clear();
        if (treeExited)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CloseHandle(job); // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE stops launcher + child
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
    errorOut = treeExited
        ? "the updated application exited without publishing its health token"
        : "timed out waiting for the updated application health token";
    return false;
}
}
