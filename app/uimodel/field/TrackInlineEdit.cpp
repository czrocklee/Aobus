// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/field/TrackInlineEdit.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <string_view>

namespace ao::uimodel
{
  TrackInlineEditResult applyTrackInlineEdit(TrackInlineEditRequest const& request, TrackInlineEditHooks const& hooks)
  {
    if (request.newText == request.oldText)
    {
      return TrackInlineEditResult{.outcome = TrackInlineEditOutcome::NoChange, .statusMessage = ""};
    }

    if (!hooks.parse || !hooks.readCurrentValue || !hooks.applyValue || !hooks.writePatch || hooks.session == nullptr)
    {
      return TrackInlineEditResult{.outcome = TrackInlineEditOutcome::NotEditable, .statusMessage = ""};
    }

    auto editValueResult = hooks.parse(request.newText);

    if (!editValueResult)
    {
      return TrackInlineEditResult{
        .outcome = TrackInlineEditOutcome::ParseRejected, .statusMessage = editValueResult.error().message};
    }

    auto patch = rt::MetadataPatch{};
    hooks.writePatch(patch, *editValueResult);

    auto const oldEditValue = hooks.readCurrentValue();
    hooks.applyValue(*editValueResult);

    auto const replyResult = hooks.session->submitMetadata(patch);

    if (!replyResult)
    {
      hooks.applyValue(oldEditValue);
      return TrackInlineEditResult{
        .outcome = TrackInlineEditOutcome::MutationRejected, .statusMessage = replyResult.error().message};
    }

    switch (replyResult->status)
    {
      case TrackAuthoringSubmitStatus::Applied:
        return TrackInlineEditResult{.outcome = TrackInlineEditOutcome::Applied, .statusMessage = ""};
      case TrackAuthoringSubmitStatus::NoOp:
        hooks.applyValue(oldEditValue);
        return TrackInlineEditResult{.outcome = TrackInlineEditOutcome::NoChange, .statusMessage = ""};
      case TrackAuthoringSubmitStatus::Stale:
        hooks.applyValue(oldEditValue);
        return TrackInlineEditResult{
          .outcome = TrackInlineEditOutcome::Stale,
          .statusMessage = "Library changed while this edit was open. Reload the value and try again."};
      case TrackAuthoringSubmitStatus::Missing:
        hooks.applyValue(oldEditValue);
        return TrackInlineEditResult{
          .outcome = TrackInlineEditOutcome::Missing, .statusMessage = "The edited track no longer exists."};
      case TrackAuthoringSubmitStatus::Unavailable:
        hooks.applyValue(oldEditValue);
        return TrackInlineEditResult{
          .outcome = TrackInlineEditOutcome::Unavailable, .statusMessage = "Library editing is currently unavailable."};
    }

    hooks.applyValue(oldEditValue);
    return TrackInlineEditResult{
      .outcome = TrackInlineEditOutcome::MutationRejected, .statusMessage = "Change could not be applied."};
  }

  bool isProtectedInlineEditText(rt::TrackField field,
                                 rt::TrackDetailSnapshot const& snap,
                                 std::string_view newText,
                                 bool protectCompositeMixedText)
  {
    if (newText == kMultipleTrackValuesText)
    {
      return true;
    }

    auto const& agg = rt::trackFieldArrayAt(snap.fields, field);
    return protectCompositeMixedText && agg.mixed && newText == kCompositeMixedTrackText;
  }
} // namespace ao::uimodel
