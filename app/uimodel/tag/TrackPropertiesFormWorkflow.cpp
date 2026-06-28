// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/uimodel/tag/TrackPropertiesFormWorkflow.h>
#include <ao/uimodel/track/TrackFieldEditPolicy.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace ao::uimodel::tag
{
  namespace
  {
    rt::TrackFieldRawValue rawValueFromEditValue(track::TrackFieldEditValue const& editValue)
    {
      if (auto const* text = std::get_if<std::string>(&editValue); text != nullptr)
      {
        return rt::TrackFieldRawValue{std::in_place_type<std::string>, *text};
      }

      if (auto const* number = std::get_if<std::uint16_t>(&editValue); number != nullptr)
      {
        return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, *number};
      }

      return rt::TrackFieldRawValue{};
    }
  } // namespace

  TrackPropertiesFormFieldState makeTrackPropertiesFormFieldState(rt::TrackField field, rt::TrackFieldRawValue rawValue)
  {
    return TrackPropertiesFormFieldState{.field = field, .originalRawValue = std::move(rawValue), .mixed = false};
  }

  bool mergeTrackPropertiesFormFieldState(TrackPropertiesFormFieldState& state, rt::TrackFieldRawValue const& rawValue)
  {
    if (state.mixed || rawValue == state.originalRawValue)
    {
      return false;
    }

    state.mixed = true;
    return true;
  }

  bool writeTrackPropertiesFormEdit(rt::MetadataPatch& patch,
                                    TrackPropertiesFormFieldState const& state,
                                    track::TrackFieldEditValue const& editValue)
  {
    if (state.mixed || !track::trackFieldCanWritePatch(state.field))
    {
      return false;
    }

    if (rawValueFromEditValue(editValue) == state.originalRawValue)
    {
      return false;
    }

    return track::writeTrackFieldPatch(patch, state.field, editValue);
  }
} // namespace ao::uimodel::tag
