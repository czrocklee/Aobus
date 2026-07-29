// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

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

  std::string scalar(std::string_view value);
  void indent(std::ostringstream& out, std::size_t spaces);
  void beginMapField(std::ostringstream& out, std::size_t spaces, std::string_view key);
  void beginSequenceField(std::ostringstream& out, std::size_t spaces, std::string_view key);
  void emptySequenceField(std::ostringstream& out, std::size_t spaces, std::string_view key);
  void scalarField(std::ostringstream& out, std::size_t spaces, std::string_view key, std::string_view value);

  template<typename Value>
    requires(std::is_arithmetic_v<Value> && !std::is_same_v<std::remove_cv_t<Value>, bool>)
  void scalarField(std::ostringstream& out, std::size_t spaces, std::string_view key, Value value)
  {
    indent(out, spaces);
    std::println(out, "{}: {}", scalar(key), value);
  }

  void boolField(std::ostringstream& out, std::size_t spaces, std::string_view key, bool value);
  void flowStringSequenceField(std::ostringstream& out,
                               std::size_t spaces,
                               std::string_view key,
                               std::vector<std::string> const& values);
  void beginSequenceMap(std::ostringstream& out, std::size_t spaces, std::string_view key, std::string_view value);
} // namespace ao::council::yaml_emit
