// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitText.h"

namespace ao::appkit
{
  NSString* nativeText(std::string_view text)
  {
    auto* const value = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    return value != nil ? value : @"";
  }

  std::string utf8(NSString* text)
  {
    auto const* const value = text.UTF8String;
    return value != nullptr ? std::string{value} : std::string{};
  }

  NSString* catalogText(i18n::MessageCatalog const& catalog, i18n::MessageId message)
  {
    return nativeText(i18n::requiredText(catalog, message));
  }

  NSString* catalogFormat(i18n::MessageCatalog const& catalog,
                          i18n::MessageId message,
                          std::initializer_list<i18n::MessageArgument> arguments)
  {
    return nativeText(i18n::requiredFormat(catalog, message, arguments));
  }
} // namespace ao::appkit
