// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstddef>
#include <iosfwd>
#include <span>
#include <string_view>

namespace ao::library
{
  class DictionaryStore;
} // namespace ao::library

namespace ao::cli
{
  void hexDump(std::span<std::byte const> data, std::ostream& os);
  std::string_view dictionaryText(library::DictionaryStore const& dictionary, DictionaryId id);
} // namespace ao::cli
