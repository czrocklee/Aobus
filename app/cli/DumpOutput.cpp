// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DumpOutput.h"

#include <ao/CoreIds.h>
#include <ao/library/DictionaryStore.h>
#include <ao/utility/String.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <print>
#include <span>
#include <string_view>

namespace ao::cli
{
  void hexDump(std::span<std::byte const> data, std::ostream& os)
  {
    std::size_t offset = 0;

    while (offset < data.size())
    {
      std::print(os, "  {:04x}  ", offset);

      std::size_t const chunk = std::min<std::size_t>(16, data.size() - offset);

      for (std::size_t i = 0; i < 16; ++i)
      {
        if (i < chunk)
        {
          std::print(os, "{:02x} ", static_cast<std::int32_t>(data[offset + i]));
        }
        else
        {
          std::print(os, "   ");
        }
      }

      std::print(os, " |");

      for (std::size_t i = 0; i < chunk; ++i)
      {
        char const charVal = static_cast<char>(data[offset + i]);
        std::print(os, "{}", utility::isAsciiPrintable(charVal) ? charVal : '.');
      }

      std::println(os, "|");

      offset += chunk;
    }
  }

  std::string_view dictionaryText(library::DictionaryStore const& dictionary, DictionaryId id)
  {
    if (id == 0)
    {
      return "";
    }

    return dictionary.getOrDefault(id, "<INVALID>");
  }
} // namespace ao::cli
