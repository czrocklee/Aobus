// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TagEditWorkflow.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>
#include <ao/uimodel/library/track/TrackCountFormatter.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    std::string tagChangeStatusMessage(std::size_t trackCount, std::size_t addedCount, std::size_t removedCount)
    {
      auto parts = std::vector<std::string>{};

      if (addedCount > 0)
      {
        parts.push_back(std::format("added {}", addedCount));
      }

      if (removedCount > 0)
      {
        parts.push_back(std::format("removed {}", removedCount));
      }

      if (parts.empty())
      {
        return std::format("Tags unchanged for {}", formatTrackCount(trackCount));
      }

      auto message = std::format("Tags {}", parts[0]);

      if (parts.size() > 1)
      {
        message += " and " + parts[1];
      }

      return std::format("{} for {}", message, formatTrackCount(trackCount));
    }
  } // namespace

  TagEditWorkflow::TagEditWorkflow(TrackAuthoringSession& session)
    : _session{session}
  {
  }

  TagEditResult TagEditWorkflow::apply(TagEditRequest const& request)
  {
    auto result = TagEditResult{};

    if (request.selectedIds.empty() || (request.tagsToAdd.empty() && request.tagsToRemove.empty()))
    {
      return result;
    }

    if (!std::ranges::equal(request.selectedIds, _session.targetIds()))
    {
      result.rejected = true;
      result.notificationText = "Tag edit targets changed while the editor was open.";
      return result;
    }

    auto const replyResult = _session.submitTags(request.tagsToAdd, request.tagsToRemove);

    if (!replyResult)
    {
      result.rejected = true;
      result.notificationText = replyResult.error().message;
      return result;
    }

    if (replyResult->status == TrackAuthoringSubmitStatus::Stale ||
        replyResult->status == TrackAuthoringSubmitStatus::Unavailable)
    {
      result.stale = true;
      result.notificationText = "Library changed while the tag editor was open. Reload and try again.";
      return result;
    }

    if (replyResult->status == TrackAuthoringSubmitStatus::Missing)
    {
      result.rejected = true;
      result.notificationText = "One or more selected tracks no longer exist.";
      return result;
    }

    result.applied = replyResult->status == TrackAuthoringSubmitStatus::Applied;
    result.notificationText = tagChangeStatusMessage(
      replyResult->reply.mutatedIds.size(), request.tagsToAdd.size(), request.tagsToRemove.size());
    return result;
  }
} // namespace ao::uimodel
