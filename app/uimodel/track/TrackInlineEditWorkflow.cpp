// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/track/TrackInlineEditWorkflow.h>

namespace ao::uimodel::track
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

    if (auto result = hooks.commitPatch(patch); !result)
    {
      hooks.applyValue(oldEditValue);
      return TrackInlineEditResult{
        .outcome = TrackInlineEditOutcome::MutationRejected, .statusMessage = result.error().message};
    }

    return TrackInlineEditResult{.outcome = TrackInlineEditOutcome::Applied, .statusMessage = ""};
  }
} // namespace ao::uimodel::track
