// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>

#include <string>
#include <string_view>

namespace ao::gtk::layout::track_field_grid
{
  std::string validUtf8Text(std::string_view text);

  bool isProtectedFieldEditValue(rt::TrackField field,
                                 rt::TrackDetailSnapshot const& snap,
                                 std::string_view newValue,
                                 std::string_view mixedText,
                                 bool requireMixedField);
} // namespace ao::gtk::layout::track_field_grid
