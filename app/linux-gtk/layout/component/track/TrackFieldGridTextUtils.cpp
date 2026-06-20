// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/track/TrackFieldGridTextUtils.h"

#include "track/TrackFieldUi.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/ProjectionTypes.h>

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

  std::string displayTextForField(rt::TrackField field,
                                  rt::TrackDetailSnapshot const& snap,
                                  std::string_view mixedText,
                                  bool showTechnicalUnknown)
  {
    auto const& agg = rt::trackFieldArrayAt(snap.fields, field);
    auto const* uiDef = trackFieldUiDefinition(field);
    auto const* def = rt::trackFieldDefinition(field);

    if (agg.mixed)
    {
      return std::string{mixedText};
    }

    if (!agg.optValue)
    {
      if (showTechnicalUnknown && def != nullptr && def->category == rt::TrackFieldCategory::Technical)
      {
        return "Unknown";
      }

      return {};
    }

    if (uiDef != nullptr && uiDef->formatValue != nullptr)
    {
      return uiDef->formatValue(*agg.optValue);
    }

    return {};
  }

  bool isProtectedFieldEditValue(rt::TrackField field,
                                 rt::TrackDetailSnapshot const& snap,
                                 std::string_view newValue,
                                 bool protectCompositeMixedText)
  {
    if (newValue == kMultipleValuesText)
    {
      return true;
    }

    auto const& agg = rt::trackFieldArrayAt(snap.fields, field);
    return protectCompositeMixedText && agg.mixed && newValue == kCompositeMixedText;
  }
} // namespace ao::gtk::layout::track_field_grid
