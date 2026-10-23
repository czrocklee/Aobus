// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "SystemLocale.h"
#include <ao/Error.h>

#include <unicode/uloc.h>
#include <unicode/utypes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace ao::i18n::detail
{
  Result<std::string> systemLocaleTag()
  {
    auto const* locale = ::uloc_getDefault();

    if (locale == nullptr || locale[0] == '\0')
    {
      return makeError(Error::Code::NotFound, "The operating system did not provide a UI locale");
    }

    auto output = std::array<char, ULOC_FULLNAME_CAPACITY>{};
    UErrorCode status = U_ZERO_ERROR;
    auto const length =
      ::uloc_toLanguageTag(locale, output.data(), static_cast<std::int32_t>(output.size()), 1, &status);

    if (U_FAILURE(status) != 0)
    {
      return makeError(
        Error::Code::InvalidInput,
        std::format("The operating-system locale is not a strict language tag: {}", ::u_errorName(status)));
    }

    return std::string{output.data(), static_cast<std::size_t>(length)};
  }
} // namespace ao::i18n::detail
