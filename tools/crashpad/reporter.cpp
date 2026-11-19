#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/prune_crash_reports.h"
#include "client/settings.h"

#include <memory>
#include <string>
#include <windows.h>

namespace
{
constexpr wchar_t werRegistryKey[] =
    L"Software\\Microsoft\\Windows\\Windows Error Reporting\\RuntimeExceptionHelperModules";

std::unique_ptr<crashpad::CrashpadClient> client;
std::wstring werModule;
}

extern "C" __declspec(dllexport) int StartSyncToolCrashReporter(const wchar_t *directory, const wchar_t *reports,
                                                               const char *version)
{
    const base::FilePath bin(directory);
    const auto handler = bin.Append(L"crashpad_handler.exe");
    const base::FilePath databasePath(reports);
    auto database = crashpad::CrashReportDatabase::Initialize(databasePath);
    if (!database || !database->GetSettings()->SetUploadsEnabled(false)) return 0;

    const auto retention = crashpad::PruneCondition::GetDefault();
    crashpad::PruneCrashReportDatabase(database.get(), retention.get());

    client = std::make_unique<crashpad::CrashpadClient>();
    if (!client->StartHandler(handler, databasePath, {}, {}, {},
                              {{"prod", "RemoteReleaseSync"}, {"ver", version}}, {}, true, false)) {
        client.reset();
        return 0;
    }

    const std::wstring module = bin.Append(L"crashpad_wer.dll").value();
    if (GetFileAttributesW(module.c_str()) == INVALID_FILE_ATTRIBUTES) return 1;
    const DWORD enabled = 1;
    if (RegSetKeyValueW(HKEY_CURRENT_USER, werRegistryKey, module.c_str(), REG_DWORD, &enabled, sizeof(enabled))
        != ERROR_SUCCESS) return 1;
    if (!client->RegisterWerModule(module)) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, werRegistryKey, module.c_str());
        return 1;
    }
    werModule = module;
    return 2;
}

extern "C" __declspec(dllexport) void StopSyncToolCrashReporter()
{
    client.reset();
    if (!werModule.empty()) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, werRegistryKey, werModule.c_str());
        werModule.clear();
    }
}
