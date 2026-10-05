#pragma once

#include <filesystem>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

// Each room has its own native engine process, private configuration and
// userdata. No player-provided text is ever interpreted by a shell.
class HostedWorkers {
public:
 struct Settings {
  std::string Executable, Directory, Address, StateDirectory;
  uint16_t PortBase = 8100;
  size_t Capacity = 0;
  unsigned IdleSeconds = 600;
  unsigned StartupSeconds = 120;
 } Config;
 struct Worker {
  std::string Code;
  uint16_t Port = 0;
  std::filesystem::path Directory;
  uint64_t Started = 0;
  bool Ready = false;
#ifdef _WIN32
  HANDLE Process = nullptr;
#else
  pid_t Process = -1;
#endif
 };
 std::vector<Worker> Workers;
#ifdef _WIN32
 HANDLE Job = nullptr;
#endif
 ~HostedWorkers() { for (auto& worker : Workers) Stop(worker);
#ifdef _WIN32
  if (Job) CloseHandle(Job);
#endif
 }
 bool Enabled() const { return Config.Capacity && !Config.Executable.empty() && !Config.Directory.empty() && !Config.Address.empty() && !Config.StateDirectory.empty(); }
 static bool SafeText(const std::string& text) { return text.find_first_of("\r\n\0", 0, 3) == std::string::npos; }
 static uint64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
 std::optional<uint16_t> Launch(const std::string& code, const std::string& owner, const std::string& room, const std::string& password) {
  if (!Enabled() || Workers.size() >= Config.Capacity || !SafeText(room) || !SafeText(password)) return {};
  uint16_t port = 0;
  for (size_t i = 0; i < Config.Capacity; ++i) {
   const auto candidate = uint16_t(Config.PortBase + i);
   if (std::none_of(Workers.begin(), Workers.end(), [candidate](const auto& worker) { return worker.Port == candidate; })) { port = candidate; break; }
  }
  if (!port) return {};
  Worker worker; worker.Code = code; worker.Port = port;
  worker.Directory = std::filesystem::absolute(std::filesystem::path(Config.StateDirectory) / ("worker-" + std::to_string(port)));
  struct ConfigCleanup { std::filesystem::path Path; bool Started = false; ~ConfigCleanup() { if (!Started) { std::error_code error; std::filesystem::remove(Path, error); } } } cleanup{worker.Directory / "server.ini"};
  std::error_code error;
  std::filesystem::create_directories(worker.Directory / "userdata", error);
  if (error) return {};
#ifndef _WIN32
  std::filesystem::permissions(worker.Directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
  if (error) return {};
#endif
  const auto config = worker.Directory / "server.ini";
  const auto status = worker.Directory / "status.txt";
  std::filesystem::remove(status, error);
  if (error) return {};
  {
   std::ofstream file(config, std::ios::trunc);
   file << "port=" << port << "\nowner_token=" << owner << "\nroom_name=" << room << "\npassword=" << password
        << "\nidle_seconds=" << Config.IdleSeconds << "\nstatus_file=" << status.generic_string() << '\n';
   if (!file) return {};
  }
#ifndef _WIN32
  std::filesystem::permissions(config, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
  if (error) return {};
#endif
  const auto settings = worker.Directory / "settings.ini";
  {
   std::ofstream file(settings, std::ios::trunc);
   file << "SettingsMan\n\tResolutionX = 960\n\tResolutionY = 540\n\tResolutionMultiplier = 1\n\tFullscreen = 0\n\tSkipIntro = 1\n\tLaunchIntoActivity = 0\n";
   const auto mods = std::filesystem::path(Config.Directory) / "Mods";
   const bool hasMods = std::filesystem::is_directory(mods, error);
   if (error && error != std::errc::no_such_file_or_directory) return {};
   error.clear();
   if (hasMods) {
    std::filesystem::directory_iterator mod(mods, error), end;
    if (error) return {};
    for (; mod != end; mod.increment(error)) {
     if (error) return {};
     const bool directory = mod->is_directory(error); if (error) return {};
     if (directory && mod->path().extension() == ".rte") file << "\tDisableMod = " << mod->path().filename().string() << '\n';
    }
    if (error) return {};
   }
   if (!file) return {};
  }
  const auto log = worker.Directory / "worker.log";
#ifdef _WIN32
  auto wide = [](const std::string& text) { const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0); std::wstring result(size, L'\0'); MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size); return result; };
  auto quote = [](const std::wstring& value) { std::wstring result = L"\""; size_t slashes = 0; for (wchar_t ch : value) { if (ch == L'\\') { ++slashes; continue; } result.append(slashes * (ch == L'\"' ? 2 : 1), L'\\'); slashes = 0; if (ch == L'\"') result += L'\\'; result += ch; } result.append(slashes * 2, L'\\'); return result + L'\"'; };
  const auto executable = wide(Config.Executable), directory = wide(Config.Directory);
  std::wstring command = quote(executable) + L" -mp-dedicated " + quote(config.wstring());
  std::vector<wchar_t> environment;
  wchar_t* inherited = GetEnvironmentStringsW();
  if (!inherited) return {};
  for (const wchar_t* entry = inherited; *entry; entry += wcslen(entry) + 1) if (_wcsnicmp(entry, L"CCCP_SETTINGSPATH=", 17) != 0 && _wcsnicmp(entry, L"CCCP_USERDATA_PATH=", 18) != 0) environment.insert(environment.end(), entry, entry + wcslen(entry) + 1);
  FreeEnvironmentStringsW(inherited);
  for (const auto& entry : {L"CCCP_SETTINGSPATH=" + settings.wstring(), L"CCCP_USERDATA_PATH=" + (worker.Directory / "userdata").wstring()}) { environment.insert(environment.end(), entry.begin(), entry.end()); environment.push_back(0); }
  environment.push_back(0);
  if (!Job) { Job = CreateJobObjectW(nullptr, nullptr); if (!Job) return {}; JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; if (!SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { CloseHandle(Job); Job = nullptr; return {}; } }
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output; startup.StartupInfo.hStdInput = input;
  SIZE_T attributeSize = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
  std::vector<uint8_t> attributeStorage(attributeSize);
  startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
  const bool initialized = attributeSize && InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize);
  HANDLE inheritedHandles[] = {output, input};
  const bool isolated = initialized && output != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr);
  PROCESS_INFORMATION info{};
  const bool started = isolated && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment.data(), directory.c_str(), &startup.StartupInfo, &info);
  if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
  if (output != INVALID_HANDLE_VALUE) CloseHandle(output); if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
  if (!started) return {};
  if (!AssignProcessToJobObject(Job, info.hProcess)) { TerminateProcess(info.hProcess, 1); CloseHandle(info.hThread); CloseHandle(info.hProcess); return {}; }
  ResumeThread(info.hThread);
  CloseHandle(info.hThread); worker.Process = info.hProcess;
#else
  std::vector<std::string> entries;
  for (char** entry = environ; *entry; ++entry) if (std::string_view(*entry).find("CCCP_SETTINGSPATH=") != 0 && std::string_view(*entry).find("CCCP_USERDATA_PATH=") != 0) entries.emplace_back(*entry);
  entries.push_back("CCCP_SETTINGSPATH=" + settings.string()); entries.push_back("CCCP_USERDATA_PATH=" + (worker.Directory / "userdata").string());
  std::vector<char*> environment; for (auto& entry : entries) environment.push_back(entry.data()); environment.push_back(nullptr);
  const std::string configArgument = config.string(); char* arguments[] = {Config.Executable.data(), const_cast<char*>("-mp-dedicated"), const_cast<char*>(configArgument.c_str()), nullptr};
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) return {};
  int setup = posix_spawn_file_actions_addchdir_np(&actions, Config.Directory.c_str());
  setup |= posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  setup |= posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  setup |= posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
  // Only the remapped standard streams belong in a worker. In particular,
  // inherited broker sockets must not keep the public service port open.
  setup |= posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
#endif
  pid_t child = -1; const int result = setup ? setup : posix_spawn(&child, Config.Executable.c_str(), &actions, nullptr, arguments, environment.data());
  posix_spawn_file_actions_destroy(&actions);
  if (result) return {};
  worker.Process = child;
#endif
  worker.Started = Now(); cleanup.Started = true; Workers.push_back(std::move(worker)); return port;
 }
 bool Ready(const std::string& code) const { const auto worker = std::find_if(Workers.begin(), Workers.end(), [&](const auto& value) { return value.Code == code; }); if (worker == Workers.end()) return false; if (worker->Ready) return true; std::ifstream status(worker->Directory / "status.txt"); std::string line; while (std::getline(status, line)) if (line == "ready=1") return true; return false; }
 std::vector<std::string> Poll() {
  std::vector<std::string> expired;
  for (auto it = Workers.begin(); it != Workers.end();) {
#ifdef _WIN32
   bool done = WaitForSingleObject(it->Process, 0) == WAIT_OBJECT_0;
#else
   bool done = waitpid(it->Process, nullptr, WNOHANG) == it->Process;
#endif
   if (!done) {
    it->Ready = Ready(it->Code);
    if (!it->Ready && Now() - it->Started >= uint64_t(Config.StartupSeconds) * 1000) { Stop(*it); done = true; }
   }
   if (!done) { ++it; continue; }
   expired.push_back(it->Code);
#ifdef _WIN32
   if (it->Process) CloseHandle(it->Process);
#endif
   std::error_code error; std::filesystem::remove(it->Directory / "server.ini", error);
   it = Workers.erase(it);
  }
  return expired;
 }
 void Stop(Worker& worker) {
#ifdef _WIN32
  if (worker.Process) { TerminateProcess(worker.Process, 0); WaitForSingleObject(worker.Process, 5000); CloseHandle(worker.Process); worker.Process = nullptr; }
#else
  if (worker.Process > 0) { kill(worker.Process, SIGTERM); for (int i = 0; i < 50; ++i) { if (waitpid(worker.Process, nullptr, WNOHANG) == worker.Process) { worker.Process = -1; break; } usleep(10000); } if (worker.Process > 0) { kill(worker.Process, SIGKILL); waitpid(worker.Process, nullptr, 0); worker.Process = -1; } }
#endif
  std::error_code error; std::filesystem::remove(worker.Directory / "server.ini", error);
 }
};
