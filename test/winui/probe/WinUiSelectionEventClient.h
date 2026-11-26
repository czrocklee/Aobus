// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <unknwn.h>
#include <windows.h>
#include <winrt/base.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace ao::winui::test
{
  // Own the external client (and the compiler used by Add-Type) without waiting
  // on the UI thread. A desktop deadline or interrupted audit closes the job.
  class SelectionEventClient final
  {
  public:
    SelectionEventClient(std::string const& runfile, std::filesystem::path const& scratch, ::HWND window);
    bool tryTakeBulkRequest();
    std::optional<::DWORD> exitCode() const;
    void log() const;

  private:
    std::filesystem::path _log;
    std::chrono::steady_clock::time_point const _deadline;
    winrt::handle _bulkReady;
    winrt::handle _process;
    winrt::handle _job;
  };
} // namespace ao::winui::test
