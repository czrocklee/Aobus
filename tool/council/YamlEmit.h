// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ao::council::yaml_emit
{
  namespace detail
  {
    constexpr auto kAsciiControlBound = 0x20;
    constexpr auto kAsciiDel = 0x7F;
    constexpr auto kAsciiLimit = 0x80;
  } // namespace detail

  inline std::string scalar(std::string_view value)
  {
    auto const plain = !value.empty() && std::ranges::all_of(value,
                                                             [](char character)
                                                             {
                                                               auto const byte = static_cast<unsigned char>(character);
                                                               return std::isalnum(byte) != 0 || character == '-' ||
                                                                      character == '_' || character == '.' ||
                                                                      character == '/' || character == '@';
                                                             });

    if (plain)
    {
      return std::string{value};
    }

    auto output = std::ostringstream{};
    std::print(output, "\"");

    for (auto const character : value)
    {
      switch (auto const byte = static_cast<unsigned char>(character); character)
      {
        case '\\': std::print(output, "\\\\"); break;
        case '"': std::print(output, "\\\""); break;
        case '\n': std::print(output, "\\n"); break;
        case '\r': std::print(output, "\\r"); break;
        case '\t': std::print(output, "\\t"); break;
        default:
          if (byte < detail::kAsciiControlBound || byte == detail::kAsciiDel || byte >= detail::kAsciiLimit)
          {
            std::print(output, "\\\\x{:02X}", byte);
          }
          else
          {
            std::print(output, "{}", character);
          }
      }
    }

    std::print(output, "\"");
    return output.str();
  }

  inline void indent(std::ostringstream& out, std::size_t spaces)
  {
    for (std::size_t index = 0; index < spaces; ++index)
    {
      out.put(' ');
    }
  }

  inline void beginMapField(std::ostringstream& out, std::size_t spaces, std::string_view key)
  {
    indent(out, spaces);
    std::println(out, "{}:", scalar(key));
  }

  inline void beginSequenceField(std::ostringstream& out, std::size_t spaces, std::string_view key)
  {
    indent(out, spaces);
    std::println(out, "{}:", scalar(key));
  }

  inline void emptySequenceField(std::ostringstream& out, std::size_t spaces, std::string_view key)
  {
    indent(out, spaces);
    std::println(out, "{}: []", scalar(key));
  }

  inline void scalarField(std::ostringstream& out, std::size_t spaces, std::string_view key, std::string_view value)
  {
    indent(out, spaces);
    std::println(out, "{}: {}", scalar(key), scalar(value));
  }

  template<typename Value>
    requires(std::is_arithmetic_v<Value> && !std::is_same_v<std::remove_cv_t<Value>, bool>)
  void scalarField(std::ostringstream& out, std::size_t spaces, std::string_view key, Value value)
  {
    indent(out, spaces);
    std::println(out, "{}: {}", scalar(key), value);
  }

  inline void boolField(std::ostringstream& out, std::size_t spaces, std::string_view key, bool value)
  {
    indent(out, spaces);
    std::println(out, "{}: {}", scalar(key), value ? "true" : "false");
  }

  inline void flowStringSequenceField(std::ostringstream& out,
                                      std::size_t spaces,
                                      std::string_view key,
                                      std::vector<std::string> const& values)
  {
    indent(out, spaces);
    std::print(out, "{}: [", scalar(key));

    for (std::size_t index = 0; index < values.size(); ++index)
    {
      std::print(out, "{}{}", index == 0 ? "" : ", ", scalar(values[index]));
    }

    std::print(out, "]\n");
  }

  inline void beginSequenceMap(std::ostringstream& out,
                               std::size_t spaces,
                               std::string_view key,
                               std::string_view value)
  {
    indent(out, spaces);
    std::println(out, "- {}: {}", scalar(key), scalar(value));
  }
} // namespace ao::council::yaml_emit
