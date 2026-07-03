// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/field/TrackInlineEditWorkflow.h>

#include <string_view>

namespace ao::uimodel
{
  TrackInlineEditResult TrackInlineEditWorkflow::apply(TrackInlineEditRequest const& request,
                                                       TrackInlineEditHooks const& hooks)
  {
    if (request.newText == request.oldText)
    {
      return TrackInlineEditResult{.outcome = TrackInlineEditOutcome::NoChange, .statusMessage = ""};
    }

    if (!hooks.parse || !hooks.readCurrentValue || !hooks.applyValue || !hooks.writePatch || !hooks.commitPatch)
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

    auto const replyResult = hooks.commitPatch(patch);

    if (!replyResult)
    {
      hooks.applyValue(oldEditValue);
      return TrackInlineEditResult{
        .outcome = TrackInlineEditOutcome::MutationRejected, .statusMessage = replyResult.error().message};
    }

    // Reaching here with no mutated tracks means the edit did not land (e.g. the
    // row was removed concurrently), so revert the optimistic UI change.
    if (replyResult->mutatedIds.empty())
    {
      hooks.applyValue(oldEditValue);
      return TrackInlineEditResult{
        .outcome = TrackInlineEditOutcome::MutationRejected, .statusMessage = "Change could not be applied."};
    }

    return TrackInlineEditResult{.outcome = TrackInlineEditOutcome::Applied, .statusMessage = ""};
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
