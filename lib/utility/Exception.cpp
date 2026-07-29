// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Exception.h>

#include <cstdint>
#include <source_location>
#include <string>
#include <utility>

namespace ao
{
  Exception::Exception(std::string what, std::source_location loc)
    : _what{std::move(what)}, _location{loc}
  {
  }

  char const* Exception::file() const noexcept
  {
    return _location.file_name();
  }

  std::int32_t Exception::line() const noexcept
  {
    return static_cast<std::int32_t>(_location.line());
  }

  char const* Exception::what() const noexcept
  {
    return _what.c_str();
  }

  std::source_location const& Exception::location() const noexcept
  {
    return _location;
  }
} // namespace ao
