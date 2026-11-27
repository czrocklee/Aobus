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

    std::unexpected<Error> contextualPersistedError(Error error, std::string_view const context)
    {
      if (error.code == Error::Code::InvalidInput || error.code == Error::Code::ValueTooLarge)
      {
        error.code = Error::Code::CorruptData;
      }

      return contextualError(std::move(error), context);
    }
  } // namespace

  Result<> validateLibraryText(std::string_view const text, std::string_view const context)
  {
    if (auto validationRes = utility::validateUtf8(text); !validationRes)
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

    return normalizeLibraryText(text, context).transform(&std::string::size);
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
      return contextualPersistedError(std::move(nfcRes.error()), context);
    }

    if (!*nfcRes)
    {
      return makeError(Error::Code::CorruptData, std::format("{} is not NFC", context));
    }

    return {};
  }

  Result<> validatePersistedLibraryUtf8(std::string_view const text, std::string_view const context)
  {
    if (auto validationRes = utility::validateUtf8(text); !validationRes)
    {
      return contextualPersistedError(std::move(validationRes.error()), context);
    }

    return {};
  }
} // namespace ao::library::detail
