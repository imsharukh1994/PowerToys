// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#define WIN32_LEAN_AND_MEAN
#include "Generated Files/resource.h"

#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string_view>
#include <common/updating/updating.h>
#include <common/updating/updateState.h>
#include <common/updating/installer.h>
#include <common/utils/elevation.h>
#include <common/utils/HttpClient.h>
#include <common/utils/process_path.h>
#include <common/utils/resources.h>
#include <common/utils/timeutil.h>
#include <common/SettingsAPI/settings_helpers.h>
#include <common/logger/logger.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <Msi.h>
#include "../runner/tray_icon.h"
#include "../runner/UpdateUtils.h"

using namespace cmdArg;
namespace fs = std::filesystem;

std::optional<fs::path> CopySelfToTempDir()
{
    std::error_code error;
    auto dst_path = fs::temp_directory_path() / "PowerToys.Update.exe";
    fs::copy_file(get_module_filename(), dst_path, fs::copy_options::overwrite_existing, error);
    if (error)
    {
        Logger::error("Failed to copy to temp directory: {}", error.message());
        return std::nullopt;
    }
    return dst_path;
}

std::optional<fs::path> ObtainInstaller(bool& isUpToDate)
{
    using namespace updating;
    isUpToDate = false;
    auto state = UpdateState::read();
    const auto new_version_info = get_github_version_info_async().get();

    if (!new_version_info)
    {
        Logger::error("Failed to retrieve version info. Check your network connection.");
        return std::nullopt;
    }

    if (std::holds_alternative<version_up_to_date>(*new_version_info))
    {
        isUpToDate = true;
        Logger::info("PowerToys is already up to date.");
        return std::nullopt;
    }

    updating::cleanup_updates();
    if (state.state == UpdateState::readyToDownload || state.state == UpdateState::errorDownloading)
    {
        auto downloaded_installer = download_new_version(std::get<new_version_download_info>(*new_version_info)).get();
        if (!downloaded_installer)
        {
            Logger::error("Failed to download new installer.");
            return std::nullopt;
        }
        return downloaded_installer;
    }

    if (state.state == UpdateState::readyToInstall)
    {
        fs::path installer = get_pending_updates_path() / state.downloadedInstallerFilename;
        if (!fs::exists(installer))
        {
            Logger::error("Installer file not found: {}", installer.native());
            return std::nullopt;
        }
        return installer;
    }

    Logger::error("Unexpected update state: {}", state.state);
    return std::nullopt;
}

bool InstallNewVersionStage1(fs::path installer)
{
    auto temp_exe = CopySelfToTempDir();
    if (!temp_exe)
    {
        return false;
    }

    const auto pt_main_window = FindWindowW(pt_tray_icon_window_class, nullptr);
    if (pt_main_window != nullptr)
    {
        SendMessageW(pt_main_window, WM_CLOSE, 0, 0);
    }

    std::wstring arguments = UPDATE_NOW_LAUNCH_STAGE2;
    arguments += L" \"" + installer.c_str() + L"\"";
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpFile = temp_exe->c_str();
    sei.nShow = SW_SHOWNORMAL;
    sei.lpParameters = arguments.c_str();

    return ShellExecuteExW(&sei) == TRUE;
}

bool InstallNewVersionStage2(std::wstring installer_path)
{
    std::transform(installer_path.begin(), installer_path.end(), installer_path.begin(), ::towlower);

    bool success = true;
    if (installer_path.ends_with(L".msi"))
    {
        success = MsiInstallProductW(installer_path.data(), nullptr) == ERROR_SUCCESS;
    }
    else
    {
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
        sei.lpFile = installer_path.c_str();
        sei.nShow = SW_SHOWNORMAL;
        sei.lpParameters = L"/passive /norestart";

        success = ShellExecuteExW(&sei) == TRUE;

        if (success)
        {
            DWORD exitCode = 0;
            WaitForSingleObject(sei.hProcess, 60000);
            GetExitCodeProcess(sei.hProcess, &exitCode);
            success = (exitCode == 0);
            CloseHandle(sei.hProcess);
        }
    }

    if (!success)
    {
        Logger::error("Installer failed: {}", installer_path);
        return false;
    }

    UpdateState::store([&](UpdateState& state) {
        state = {};
        state.githubUpdateLastCheckedDate.emplace(timeutil::now());
        state.state = UpdateState::upToDate;
    });

    return true;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    int nArgs = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (!args || nArgs < 2)
    {
        Logger::error("Invalid command-line arguments.");
        return 1;
    }

    std::wstring_view action{ args[1] };
    Logger::init(LogSettings::updateLoggerName, 
                 PTSettingsHelper::get_log_file_location(),
                 PTSettingsHelper::get_log_settings_file_location());

    if (action == UPDATE_NOW_LAUNCH_STAGE1)
    {
        bool isUpToDate = false;
        auto installer = ObtainInstaller(isUpToDate);
        if (!installer || !InstallNewVersionStage1(*installer))
        {
            UpdateState::store([&](UpdateState& state) {
                state = {};
                state.githubUpdateLastCheckedDate.emplace(timeutil::now());
                state.state = isUpToDate ? UpdateState::upToDate : UpdateState::errorDownloading;
            });
        }
        return 0;
    }
    else if (action == UPDATE_NOW_LAUNCH_STAGE2)
    {
        if (!InstallNewVersionStage2(args[2]))
        {
            UpdateState::store([&](UpdateState& state) {
                state = {};
                state.githubUpdateLastCheckedDate.emplace(timeutil::now());
                state.state = UpdateState::errorDownloading;
            });
        }
        return 0;
    }

    Logger::error("Invalid action specified: {}", action);
    return 1;
}