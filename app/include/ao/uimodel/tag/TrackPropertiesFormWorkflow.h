// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

namespace ao::uimodel::tag
{
  struct TrackPropertiesFormFieldState final
  {
    rt::TrackField field = rt::TrackField::Title;
    rt::TrackFieldRawValue originalRawValue{};
    bool mixed = false;
  };

  TrackPropertiesFormFieldState makeTrackPropertiesFormFieldState(rt::TrackField field,
                                                                  rt::TrackFieldRawValue rawValue);
  bool mergeTrackPropertiesFormFieldState(TrackPropertiesFormFieldState& state, rt::TrackFieldRawValue const& rawValue);
  bool writeTrackPropertiesFormEdit(rt::MetadataPatch& patch,
                                    TrackPropertiesFormFieldState const& state,
                                    track::TrackFieldEditValue const& editValue);
} // namespace ao::uimodel::tag
