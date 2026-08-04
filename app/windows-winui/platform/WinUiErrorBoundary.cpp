// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/WinUiErrorBoundary.h>

#include <ao/rt/Log.h>

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <windows.h>
#include <winrt/base.h>

#include <cstdint>
#include <source_location>
#include <string_view>

namespace ao::winui
{
  namespace
  {
    constexpr auto kDiagnosticFallback = "Aobus could not write a WinUI diagnostic.\n";

    void writeDiagnosticFallback() noexcept
    {
      ::OutputDebugStringA(kDiagnosticFallback);
    }
  } // namespace

  void reportOptionalWinRtFailure(std::string_view const context,
                                  winrt::hresult_error const& error,
                                  std::source_location const location) noexcept
  {
    try
    {
      auto const& loggerPtr = rt::Log::appLogger();

      if (loggerPtr == nullptr || !loggerPtr->should_log(spdlog::level::warn))
      {
        writeDiagnosticFallback();
        return;
      }

      loggerPtr->log(rt::toSpdlog(location),
                     spdlog::level::warn,
                     "Optional WinRT operation failed in {}: {} (HRESULT 0x{:08X})",
                     context,
                     winrt::to_string(error.message()),
                     static_cast<std::uint32_t>(error.code().value));
    }
    catch (...)
    {
      writeDiagnosticFallback();
    }
  }

  void logWinUiCritical(std::string_view const context,
                        std::string_view const detail,
                        std::source_location const location) noexcept
  {
    try
    {
      auto const& loggerPtr = rt::Log::appLogger();

      if (loggerPtr == nullptr || !loggerPtr->should_log(spdlog::level::critical))
      {
        writeDiagnosticFallback();
        return;
      }

      loggerPtr->log(rt::toSpdlog(location), spdlog::level::critical, "{}: {}", context, detail);
    }
    catch (...)
    {
      writeDiagnosticFallback();
    }
  }
} // namespace ao::winui
