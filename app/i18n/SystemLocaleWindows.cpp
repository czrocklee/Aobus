// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "SystemLocale.h"
#include <ao/Error.h>

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>

namespace ao::i18n::detail
{
  namespace
  {
    Result<std::string> utf8FromWide(std::wstring const& text)
    {
      if (text.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
      {
        return makeError(Error::Code::ValueTooLarge, "The operating-system locale name is too large");
      }

      auto const sourceLength = static_cast<std::int32_t>(text.size());
      auto const required =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), sourceLength, nullptr, 0, nullptr, nullptr);

      if (required <= 0)
      {
        return makeError(
          Error::Code::InvalidInput,
          std::format("Could not encode the operating-system locale as UTF-8: Win32 error {}", ::GetLastError()));
      }

      auto result = std::string(static_cast<std::size_t>(required), '\0');

      if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), sourceLength, result.data(), required, nullptr, nullptr) !=
          required)
      {
        return makeError(
          Error::Code::InvalidInput,
          std::format("Could not encode the operating-system locale as UTF-8: Win32 error {}", ::GetLastError()));
      }

      return result;
    }
  } // namespace

  Result<std::string> systemLocaleTag()
  {
    ULONG languageCount = 0;
    ULONG bufferLength = 0;

    if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &languageCount, nullptr, &bufferLength) == FALSE)
    {
      return makeError(
        Error::Code::InitFailed,
        std::format("Could not query the preferred Windows UI languages: Win32 error {}", ::GetLastError()));
    }

    if (languageCount == 0 || bufferLength <= 1)
    {
      return makeError(Error::Code::NotFound, "Windows did not provide a preferred UI language");
    }

    auto buffer = std::wstring(static_cast<std::size_t>(bufferLength), L'\0');

    if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &languageCount, buffer.data(), &bufferLength) == FALSE)
    {
      return makeError(
        Error::Code::InitFailed,
        std::format("Could not read the preferred Windows UI languages: Win32 error {}", ::GetLastError()));
    }

    auto const firstEnd = std::ranges::find(buffer, L'\0');

    if (firstEnd == buffer.begin())
    {
      return makeError(Error::Code::NotFound, "Windows returned an empty preferred UI language");
    }

    buffer.resize(static_cast<std::size_t>(firstEnd - buffer.begin()));
    return utf8FromWide(buffer);
  }
} // namespace ao::i18n::detail
