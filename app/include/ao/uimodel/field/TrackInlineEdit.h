// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ao::rt
{
  struct MetadataPatch;
}

namespace ao::uimodel
{
  class TrackAuthoringSession;

  enum class TrackInlineEditOutcome : std::uint8_t
  {
    NoChange,
    NotEditable,
    ParseRejected,
    MutationRejected,
    Stale,
    Missing,
    Unavailable,
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
    TrackAuthoringSession* session = nullptr;
  };

  TrackInlineEditResult applyTrackInlineEdit(TrackInlineEditRequest const& request, TrackInlineEditHooks const& hooks);

  bool isProtectedInlineEditText(rt::TrackField field,
                                 rt::TrackDetailSnapshot const& snap,
                                 std::string_view newText,
                                 bool protectCompositeMixedText);
} // namespace ao::uimodel
