// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#import <Foundation/Foundation.h>

#include <initializer_list>
#include <string>
#include <string_view>

namespace ao::appkit
{
  NSString* nativeText(std::string_view text);
  std::string utf8(NSString* text);
  NSString* catalogText(i18n::MessageCatalog const& catalog, i18n::MessageId message);
  NSString* catalogFormat(i18n::MessageCatalog const& catalog,
                          i18n::MessageId message,
                          std::initializer_list<i18n::MessageArgument> arguments);
}
