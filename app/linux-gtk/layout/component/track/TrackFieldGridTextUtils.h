// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/rt/projection/ProjectionTypes.h>

#include <string>
#include <string_view>

namespace ao::gtk::layout::track_field_grid
{
  inline constexpr std::string_view kMultipleValuesText = "<Multiple Values>";
  inline constexpr std::string_view kCompositeMixedText = "-";

  std::string validUtf8Text(std::string_view text);

  std::string displayTextForField(rt::TrackField field,
                                  rt::TrackDetailSnapshot const& snap,
                                  std::string_view mixedText,
                                  bool showTechnicalUnknown);

  bool isProtectedFieldEditValue(rt::TrackField field,
                                 rt::TrackDetailSnapshot const& snap,
                                 std::string_view newValue,
                                 bool protectCompositeMixedText);
} // namespace ao::gtk::layout::track_field_grid
