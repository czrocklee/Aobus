// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "WinUiSelectionEventClient.h"

#include <ao/utility/Path.h>

#include <rules_cc/cc/runfiles/runfiles.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ao::winui::test
{
  namespace
  {
    [[noreturn]] void throwWindowsError(char const* action)
    {
      throw std::system_error{static_cast<std::int32_t>(::GetLastError()), std::system_category(), action};
    }

    winrt::handle openFile(std::filesystem::path const& path,
                           ::DWORD access,
                           ::DWORD disposition,
                           SECURITY_ATTRIBUTES& attributes)
    {
      auto* const handle = ::CreateFileW(path.c_str(), access, FILE_SHARE_READ, &attributes, disposition, 0, nullptr);

      if (handle == INVALID_HANDLE_VALUE)
      {
        throwWindowsError("Could not open redirected I/O for the selection event client");
      }

      return winrt::handle{handle};
    }
  } // namespace

  SelectionEventClient::SelectionEventClient(std::string const& runfile,
                                             std::filesystem::path const& scratch,
                                             ::HWND window)
    : _log{scratch / "selection-event-client.log"}
    , _deadline{std::chrono::steady_clock::now() + std::chrono::seconds{30}}
  {
    using rules_cc::cc::runfiles::Runfiles;
    auto error = std::string{};
    auto runfilesPtr = std::unique_ptr<Runfiles>{Runfiles::CreateForTest(BAZEL_CURRENT_REPOSITORY, &error)};

    if (!runfilesPtr)
    {
      throw std::runtime_error{"Cannot resolve the selection event client runfile: " + error};
    }

    auto const script = ao::utility::pathFromUtf8(runfilesPtr->Rlocation(runfile));

    if (!script.is_absolute() || !std::filesystem::is_regular_file(script))
    {
      throw std::runtime_error{"Selection event client runfile is missing: " + runfile};
    }

    auto systemDirectory = std::array<wchar_t, MAX_PATH>{};
    auto const length = ::GetSystemDirectoryW(systemDirectory.data(), static_cast<::UINT>(systemDirectory.size()));

    if (length == 0 || length >= systemDirectory.size())
    {
      throw std::runtime_error{"Could not locate Windows PowerShell for the selection event client"};
    }

    auto const powershell = std::filesystem::path{systemDirectory.data()} / L"WindowsPowerShell/v1.0/powershell.exe";
    auto const pid = ::GetCurrentProcessId();
    auto const readyEvent = std::format(L"Local\\AobusBulkSelectionClient-{}", pid);
    _bulkReady = winrt::handle{::CreateEventW(nullptr, FALSE, FALSE, readyEvent.c_str())};

    if (!_bulkReady || ::GetLastError() == ERROR_ALREADY_EXISTS)
    {
      throwWindowsError("CreateEventW for selection event client readiness");
    }

    auto command = std::format(L"\"{}\" -NoProfile -NonInteractive -Mta -ExecutionPolicy RemoteSigned "
                               L"-File \"{}\" -WindowHandle {} -HostProcessId {} -BulkReadyEvent \"{}\"",
                               powershell.native(),
                               script.native(),
                               reinterpret_cast<std::uintptr_t>(window),
                               pid,
                               readyEvent);

    _job = winrt::handle{::CreateJobObjectW(nullptr, nullptr)};
    auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!_job || ::SetInformationJobObject(_job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == 0)
    {
      throwWindowsError("Could not create the selection event client's kill-on-close job");
    }

    auto inheritable = SECURITY_ATTRIBUTES{
      .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
    auto const output = openFile(_log, GENERIC_WRITE, CREATE_NEW, inheritable);
    auto const input = openFile(L"NUL", GENERIC_READ, OPEN_EXISTING, inheritable);
    auto startup = STARTUPINFOW{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.get();
    startup.hStdOutput = output.get();
    startup.hStdError = output.get();
    auto process = PROCESS_INFORMATION{};

    if (::CreateProcessW(powershell.c_str(),
                         command.data(),
                         nullptr,
                         nullptr,
                         TRUE,
                         CREATE_NO_WINDOW | CREATE_SUSPENDED,
                         nullptr,
                         nullptr,
                         &startup,
                         &process) == 0)
    {
      throwWindowsError("Could not start the external selection event client");
    }

    _process = winrt::handle{process.hProcess};
    auto const thread = winrt::handle{process.hThread};

    if (::AssignProcessToJobObject(_job.get(), _process.get()) == 0)
    {
      auto const errorCode = ::GetLastError();
      // Still suspended; never return an unowned process.
      [[maybe_unused]] auto const terminated = ::TerminateProcess(_process.get(), 1);
      throw std::system_error{
        static_cast<std::int32_t>(errorCode), std::system_category(), "Could not own the UIA client"};
    }

    if (::ResumeThread(thread.get()) == static_cast<::DWORD>(-1))
    {
      throwWindowsError("Could not resume the external selection event client");
    }
  }

  bool SelectionEventClient::tryTakeBulkRequest()
  {
    if (_bulkReady && ::WaitForSingleObject(_bulkReady.get(), 0) == WAIT_OBJECT_0)
    {
      _bulkReady = winrt::handle{};
      return true;
    }

    return false;
  }

  std::optional<::DWORD> SelectionEventClient::exitCode() const
  {
    auto const status = ::WaitForSingleObject(_process.get(), 0);

    if (status == WAIT_TIMEOUT && std::chrono::steady_clock::now() < _deadline)
    {
      return std::nullopt;
    }

    if (status == WAIT_TIMEOUT)
    {
      throw std::runtime_error{"External selection event client did not exit within 30 seconds"};
    }

    ::DWORD result = 1;

    if (status != WAIT_OBJECT_0 || ::GetExitCodeProcess(_process.get(), &result) == 0)
    {
      throwWindowsError("Could not read the external selection event client's exit status");
    }

    return result;
  }

  void SelectionEventClient::log() const
  {
    auto stream = std::ifstream{_log};

    for (auto line = std::string{}; std::getline(stream, line);)
    {
      std::cerr << line << '\n';
    }
  }
} // namespace ao::winui::test
