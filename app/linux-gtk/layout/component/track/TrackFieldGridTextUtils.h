// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/rt/projection/ProjectionTypes.h>

#include <string>
#include <string_view>

namespace ao::gtk::layout::track_field_grid
{
  std::string validUtf8Text(std::string_view text);

  bool isProtectedFieldEditValue(rt::TrackField field,
                                 rt::TrackDetailSnapshot const& snap,
                                 std::string_view newValue,
                                 bool protectCompositeMixedText);
} // namespace ao::gtk::layout::track_field_grid
