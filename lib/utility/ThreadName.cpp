// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/ThreadName.h>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>

namespace ao
{
  void setCurrentThreadName(std::string_view name) noexcept
  {
    try
    {
      if (name.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
      {
        return;
      }

      auto const utf8Name = std::string{name};
      auto const sourceSize = static_cast<std::int32_t>(utf8Name.size());
      auto const wideSize = ::MultiByteToWideChar(CP_UTF8, 0, utf8Name.c_str(), sourceSize, nullptr, 0);

      if (wideSize <= 0)
      {
        return;
      }

      auto wideName = std::wstring(static_cast<std::size_t>(wideSize), L'\0');
      auto const convertedSize =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8Name.c_str(), sourceSize, wideName.data(), wideSize);

      if (convertedSize == wideSize)
      {
        std::ignore = ::SetThreadDescription(::GetCurrentThread(), wideName.c_str());
      }
    }
    catch (std::exception const&)
    {
      // Thread naming is best-effort; allocation failure must not affect the worker.
      return;
    }
  }
} // namespace ao

#elifdef __linux__

#include <pthread.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace ao
{
  void setCurrentThreadName(std::string_view name) noexcept
  {
    auto buf = std::array<char, 16>{};
    std::size_t const len = std::min(name.size(), buf.size() - 1);
    std::memcpy(buf.data(), name.data(), len);
    ::pthread_setname_np(::pthread_self(), buf.data());
  }
} // namespace ao

#else

namespace ao
{
  void setCurrentThreadName(std::string_view name) noexcept
  {
    static_cast<void>(name);
  }
} // namespace ao

#endif
