#pragma once

#ifdef _WIN32
#include <windows.h>
#include <filesystem>
#include <string>

namespace RTE {
// Packaged Windows games hand startup to the updater before loading game data.
// Its child consumes the one-launch marker; development and native fixtures
// continue directly when the updater is absent or explicitly bypassed.
inline bool StartGameUpdater(int argc, char** argv) {
    if (GetEnvironmentVariableW(L"CCCP_UPDATER_LAUNCH", nullptr, 0)) {
        SetEnvironmentVariableW(L"CCCP_UPDATER_LAUNCH", nullptr);
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--no-update" || arg == "-mp-dedicated" || arg == "--mp-dedicated" || arg.starts_with("-mp-smoke-")) return false;
    }
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!size || size >= path.size()) return false;
    path.resize(size);
    const auto root = std::filesystem::path(path).parent_path();
    const auto updater = root / L"CortexUpdater.exe";
    if (!std::filesystem::is_regular_file(updater) || !std::filesystem::is_regular_file(root / L"updater.json")) return false;
    std::wstring command = L"\"" + updater.wstring() + L"\" --root \"" + root.wstring() + L"\" --parent " + std::to_wstring(GetCurrentProcessId()) + L" -- " + GetCommandLineW();
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(updater.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return true;
}
}
#endif
