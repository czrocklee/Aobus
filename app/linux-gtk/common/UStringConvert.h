// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <glibmm/ustring.h>

#include <string_view>

namespace ao::gtk
{
  // Use the iterator constructor (begin/end) rather than ustring(const char*, n),
  // which counts UTF-8 characters and would over-read when given a byte length.
  inline Glib::ustring toUString(std::string_view value)
  {
    return Glib::ustring{value.begin(), value.end()};
  }
} // namespace ao::gtk