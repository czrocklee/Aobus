// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/backend/detail/WasapiStrings.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::audio::backend::detail
{
  std::wstring utf8ToWide(std::string_view text)
  {
    if (text.empty())
    {
      return {};
    }

    std::int32_t const sourceLength = static_cast<std::int32_t>(text.size());
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): The API receives the explicit source length.
    auto const wideLength = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceLength, nullptr, 0);

    if (wideLength <= 0)
    {
      return {};
    }

    auto wide = std::wstring(static_cast<std::size_t>(wideLength), L'\0');
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): The API receives the explicit source length.
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceLength, wide.data(), wideLength);
    return wide;
  }

  std::string wideToUtf8(std::wstring_view text)
  {
    if (text.empty())
    {
      return {};
    }

    std::int32_t const sourceLength = static_cast<std::int32_t>(text.size());
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): The API receives the explicit source length.
    auto const utf8Length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), sourceLength, nullptr, 0, nullptr, nullptr);

    if (utf8Length <= 0)
    {
      return {};
    }

    auto utf8 = std::string(static_cast<std::size_t>(utf8Length), '\0');
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): The API receives the explicit source length.
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), sourceLength, utf8.data(), utf8Length, nullptr, nullptr);
    return utf8;
  }
} // namespace ao::audio::backend::detail
