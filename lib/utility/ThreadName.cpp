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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

namespace ao
{
  void setCurrentThreadName(std::string_view name) noexcept
  {
    auto wideName = std::array<wchar_t, 64>{};
    auto const sourceSize = static_cast<std::int32_t>(std::min(name.size(), wideName.size() - 1));
    auto const convertedSize = ::MultiByteToWideChar(
      CP_UTF8, 0, name.data(), sourceSize, wideName.data(), static_cast<std::int32_t>(wideName.size() - 1));

    if (convertedSize > 0)
    {
      wideName[static_cast<std::size_t>(convertedSize)] = L'\0';
      std::ignore = ::SetThreadDescription(::GetCurrentThread(), wideName.data());
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
