// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <array>
#include <cstddef>
#include <string_view>

namespace ao::i18n::detail
{
  struct MessageDefinition final
  {
    MessageId id;
    std::string_view key;
  };

  constexpr auto kMessageDefinitions = []
  {
    auto definitions = std::array<MessageDefinition, static_cast<std::size_t>(MessageId::Count)>{};
    std::size_t index = 0;
    // NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define AO_I18N_MESSAGE(id, key) definitions[index++] = MessageDefinition{MessageId::id, key};
    // NOLINTEND(cppcoreguidelines-macro-usage)
#include <ao/i18n/MessageInventory.def> // NOLINT(aobus-include-convention)
#undef AO_I18N_MESSAGE
    return definitions;
  }();

  constexpr std::size_t kMessageDefinitionCount = kMessageDefinitions.size();

  constexpr std::size_t messageIndex(MessageId const id) noexcept
  {
    return static_cast<std::size_t>(id);
  }

  static_assert(
    []
    {
      for (std::size_t index = 0; index < kMessageDefinitionCount; ++index)
      {
        if (messageIndex(kMessageDefinitions[index].id) != index)
        {
          return false;
        }
      }

      return true;
    }());

  static_assert(kMessageDefinitionCount == messageIndex(MessageId::Count));
} // namespace ao::i18n::detail
