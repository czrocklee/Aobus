// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/property/TagEdit.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>
#include <ao/uimodel/library/track/TrackCountFormatter.h>

#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <string>

namespace ao::uimodel
{
  namespace
  {
    std::string tagChangeStatusMessage(std::size_t trackCount, std::size_t addedCount, std::size_t removedCount)
    {
      auto message = std::string{"Tags "};

      if (addedCount > 0)
      {
        message += std::format("added {}", addedCount);
      }

      if (addedCount > 0 && removedCount > 0)
      {
        message += " and ";
      }

      if (removedCount > 0)
      {
        message += std::format("removed {}", removedCount);
      }

      return std::format("{} for {}", message, formatTrackCount(trackCount));
    }
  } // namespace

  Result<TagEditResult> applyTagEdit(TrackAuthoringSession& session,
                                     std::span<std::string const> tagsToAdd,
                                     std::span<std::string const> tagsToRemove)
  {
    if (tagsToAdd.empty() && tagsToRemove.empty())
    {
      return TagEditResult{};
    }

    auto replyResult = session.submitTags(tagsToAdd, tagsToRemove);

    if (!replyResult)
    {
      return std::unexpected{replyResult.error()};
    }

    switch (replyResult->status)
    {
      case rt::TrackAuthoringStatus::Applied:
        return TagEditResult{
          .status = replyResult->status,
          .notificationText =
            tagChangeStatusMessage(replyResult->reply.changes.size(), tagsToAdd.size(), tagsToRemove.size()),
        };
      case rt::TrackAuthoringStatus::NoOp: return TagEditResult{};
      case rt::TrackAuthoringStatus::Stale:
      case rt::TrackAuthoringStatus::Unavailable:
        return TagEditResult{
          .status = replyResult->status,
          .notificationText = "Library changed while the tag editor was open. Reload and try again.",
        };
      case rt::TrackAuthoringStatus::Missing:
        return TagEditResult{
          .status = replyResult->status,
          .notificationText = "One or more selected tracks no longer exist.",
        };
    }

    return makeError(Error::Code::InvalidState, "Tag edit returned an unknown authoring status");
  }
} // namespace ao::uimodel
