// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "ao/Type.h"
#include "ao/library/DictionaryStore.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iomanip>
#include <ios>
#include <ostream>
#include <span>
#include <string_view>

namespace ao::cli
{
  inline void hexDump(std::span<std::byte const> data, std::ostream& os)
  {
    std::size_t offset = 0;

    while (offset < data.size())
    {
      os << "  " << std::hex << std::setw(4) << std::setfill('0') << offset << "  ";

      std::size_t const chunk = std::min<std::size_t>(16, data.size() - offset);

      for (std::size_t i = 0; i < 16; ++i)
      {
        if (i < chunk)
        {
          os << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[offset + i]) << " ";
        }
        else
        {
          os << "   ";
        }
      }

      os << " |";

      for (std::size_t i = 0; i < chunk; ++i)
      {
        char const charVal = static_cast<char>(data[offset + i]);
        os << (std::isprint(static_cast<unsigned char>(charVal)) != 0 ? charVal : '.');
      }

      os << "|\n";

      offset += chunk;
    }

    os << std::dec << std::setfill(' '); // reset format
  }

  inline std::string_view resolveDict(library::DictionaryStore const& dict, DictionaryId id)
  {
    if (id == 0)
    {
      return "";
    }

    return dict.getOrDefault(id, "<INVALID>");
  }
} // namespace ao::cli
