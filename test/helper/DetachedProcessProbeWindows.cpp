// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

namespace
{
  std::string utf8(std::wstring_view const value)
  {
    if (value.empty())
    {
      return {};
    }

    auto const size = ::WideCharToMultiByte(CP_UTF8,
                                            WC_ERR_INVALID_CHARS,
                                            value.data(),
                                            static_cast<std::int32_t>(value.size()),
                                            nullptr,
                                            0,
                                            nullptr,
                                            nullptr);
    auto result = std::string(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8,
                          WC_ERR_INVALID_CHARS,
                          value.data(),
                          static_cast<std::int32_t>(value.size()),
                          result.data(),
                          size,
                          nullptr,
                          nullptr);
    return result;
  }
} // namespace

// The MSVC CRT requires this exact external entry-point signature.
// NOLINTNEXTLINE(aobus-modernize-use-std-numbers,misc-use-internal-linkage)
int wmain(int argumentCount, wchar_t** arguments)
{
  auto* const eventHandle = argumentCount >= 3 ? ::OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[2]) : nullptr;
  auto const outputPath = argumentCount >= 2 ? std::filesystem::path{arguments[1]} : std::filesystem::path{};
  auto const probeValue = argumentCount >= 4 ? std::wcstoull(arguments[3], nullptr, 10) : 0;
  // The probe intentionally reconstructs a foreign-process handle value to test whether it is unusable.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  auto* const probeHandle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(probeValue));
  DWORD flags = 0;
  auto const inherited = argumentCount >= 4 && ::GetHandleInformation(probeHandle, &flags) != FALSE;

  if (inherited)
  {
    DWORD written = 0;
    constexpr char kProbe = 'x';
    ::WriteFile(probeHandle, &kProbe, 1, &written, nullptr);
  }

  {
    auto output = std::ofstream{outputPath, std::ios::binary};

    if (!output)
    {
      if (eventHandle != nullptr)
      {
        ::SetEvent(eventHandle);
        ::CloseHandle(eventHandle);
      }

      return 3;
    }

    output << "argc=" << argumentCount << '\n';
    output << "inherited=" << (inherited ? '1' : '0') << '\n';

    for (std::int32_t index = 1; index < argumentCount; ++index)
    {
      output << index << '=' << utf8(arguments[index]) << '\n';
    }
  }

  if (eventHandle != nullptr)
  {
    ::SetEvent(eventHandle);
    ::CloseHandle(eventHandle);
  }

  return 0;
}
