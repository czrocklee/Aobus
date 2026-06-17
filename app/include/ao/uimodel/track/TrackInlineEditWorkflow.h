// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ao::uimodel::track
{
  enum class TrackInlineEditOutcome : std::uint8_t
  {
    NoChange,
    NotEditable,
    ParseRejected,
    MutationRejected,
    Applied,
  };

  struct TrackInlineEditResult final
  {
    TrackInlineEditOutcome outcome = TrackInlineEditOutcome::NoChange;
    std::string statusMessage;
  };

  struct TrackInlineEditRequest final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::string oldText;
    std::string newText;
  };

  struct TrackInlineEditHooks final
  {
    std::function<Result<TrackFieldEditValue>(std::string_view)> parse;
    std::function<TrackFieldEditValue()> readCurrentValue;
    std::function<void(TrackFieldEditValue const&)> applyValue;
    std::function<void(rt::MetadataPatch&, TrackFieldEditValue const&)> writePatch;
    std::function<Result<rt::UpdateTrackMetadataReply>(rt::MetadataPatch const&)> commitPatch;
  };

  class TrackInlineEditWorkflow final
  {
  public:
    static TrackInlineEditResult apply(TrackInlineEditRequest const& request, TrackInlineEditHooks const& hooks);
  };
} // namespace ao::uimodel::track
