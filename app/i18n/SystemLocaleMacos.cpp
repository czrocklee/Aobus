// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "SystemLocale.h"
#include <ao/Error.h>
#include <ao/utility/Raii.h>

#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFLocale.h>
#include <CoreFoundation/CFString.h>
#include <unicode/uloc.h>

#include <array>
#include <string>

namespace ao::i18n::detail
{
  // An application started from Finder or the Dock normally inherits no LANG
  // from launchd, so the ICU default locale would be English; the preferred
  // languages in System Settings are the macOS UI language.
  Result<std::string> systemLocaleTag()
  {
    auto const languagesPtr = utility::makeUniquePtr<::CFRelease>(::CFLocaleCopyPreferredLanguages());

    if (languagesPtr == nullptr || ::CFArrayGetCount(languagesPtr.get()) == 0)
    {
      return makeError(Error::Code::NotFound, "macOS did not provide a preferred language");
    }

    auto const* const value = ::CFArrayGetValueAtIndex(languagesPtr.get(), 0);

    if (::CFGetTypeID(value) != ::CFStringGetTypeID())
    {
      return makeError(Error::Code::InvalidInput, "The first preferred macOS language is not a string");
    }

    auto output = std::array<char, ULOC_FULLNAME_CAPACITY>{};

    auto const converted = ::CFStringGetCString(
      static_cast<::CFStringRef>(value), output.data(), static_cast<::CFIndex>(output.size()), ::kCFStringEncodingUTF8);

    if (converted == 0U)
    {
      return makeError(Error::Code::InvalidInput,
                       "Could not encode the preferred macOS language as UTF-8 within the locale-name capacity");
    }

    return std::string{output.data()};
  }
} // namespace ao::i18n::detail
