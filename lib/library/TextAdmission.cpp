// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TextAdmission.h"

#include <ao/Error.h>
#include <ao/utility/UnicodeText.h>

#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::library::detail
{
  namespace
  {
    std::unexpected<Error> contextualError(Error error, std::string_view const context)
    {
      error.message = std::format("{}: {}", context, error.message);
      return std::unexpected{std::move(error)};
    }
  } // namespace

  Result<> validateLibraryText(std::string_view const text, std::string_view const context)
  {
    auto validationRes = utility::validateUtf8(text);
    if (!validationRes)
    {
      return contextualError(std::move(validationRes.error()), context);
    }
    return {};
  }

  Result<std::size_t> normalizedLibraryTextSize(std::string_view const text, std::string_view const context)
  {
    auto nfcRes = utility::isUtf8Nfc(text);
    if (!nfcRes)
    {
      return contextualError(std::move(nfcRes.error()), context);
    }
    if (*nfcRes)
    {
      return text.size();
    }

    auto normalizedRes = utility::normalizeUtf8Nfc(text);
    if (!normalizedRes)
    {
      return contextualError(std::move(normalizedRes.error()), context);
    }
    return normalizedRes->size();
  }

  Result<std::string> normalizeLibraryText(std::string_view const text, std::string_view const context)
  {
    auto normalizedRes = utility::normalizeUtf8Nfc(text);
    if (!normalizedRes)
    {
      return contextualError(std::move(normalizedRes.error()), context);
    }
    return std::move(*normalizedRes);
  }

  Result<> validatePersistedLibraryText(std::string_view const text, std::string_view const context)
  {
    auto nfcRes = utility::isUtf8Nfc(text);
    if (!nfcRes)
    {
      auto error = std::move(nfcRes.error());
      if (error.code == Error::Code::InvalidInput || error.code == Error::Code::ValueTooLarge)
      {
        error.code = Error::Code::CorruptData;
      }
      return contextualError(std::move(error), context);
    }

    if (!*nfcRes)
    {
      return makeError(Error::Code::CorruptData, std::format("{} is not NFC", context));
    }
    return {};
  }

  Result<> validatePersistedLibraryUtf8(std::string_view const text, std::string_view const context)
  {
    auto validationRes = utility::validateUtf8(text);
    if (!validationRes)
    {
      auto error = std::move(validationRes.error());
      if (error.code == Error::Code::InvalidInput || error.code == Error::Code::ValueTooLarge)
      {
        error.code = Error::Code::CorruptData;
      }
      return contextualError(std::move(error), context);
    }
    return {};
  }
} // namespace ao::library::detail
