// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/track/TrackFieldGridText.h"

#include <ao/rt/TrackField.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/field/TrackInlineEditWorkflow.h>

#include <glib.h>
#include <glibmm/miscutils.h>

#include <string>
#include <string_view>

namespace ao::gtk::layout::track_field_grid
{
  std::string validUtf8Text(std::string_view text)
  {
    if (text.empty())
    {
      return {};
    }

    if (::g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr) != 0)
    {
      return std::string{text};
    }

    auto validPtr = Glib::make_unique_ptr_gfree(::g_utf8_make_valid(text.data(), static_cast<gssize>(text.size())));

    if (!validPtr)
    {
      return {};
    }

    return std::string{validPtr.get()};
  }

  bool isProtectedFieldEditValue(rt::TrackField field,
                                 rt::TrackDetailSnapshot const& snap,
                                 std::string_view newValue,
                                 bool protectCompositeMixedText)
  {
    return uimodel::isProtectedInlineEditText(field, snap, newValue, protectCompositeMixedText);
  }
} // namespace ao::gtk::layout::track_field_grid
