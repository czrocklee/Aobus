// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/StringResources.h"
#include <ao/utility/String.h>

#include <string>
#include <string_view>

namespace ao::winui
{
  std::string stableResourceString(std::string_view const prefix,
                                   std::string_view const stableId,
                                   std::string_view const fallback)
  {
    auto key = std::string{prefix};
    key.reserve(prefix.size() + stableId.size());

    for (auto const character : stableId)
    {
      key.push_back(utility::isAsciiAlphaNumeric(character) ? character : '_');
    }

    return resourceStringOr(key, fallback);
  }
} // namespace ao::winui
