// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TagEdit.h>

#include <ao/Error.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>
#include <ao/uimodel/library/track/TrackCountFormatter.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <utility>

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

    auto replyRes = session.submitTags(tagsToAdd, tagsToRemove);

    if (!replyRes)
    {
      return std::unexpected{replyRes.error()};
    }

    switch (replyRes->status)
    {
      case rt::TrackAuthoringStatus::Applied:
        return TagEditResult{
          .status = replyRes->status,
          .notificationText =
            tagChangeStatusMessage(replyRes->reply.changes.size(), tagsToAdd.size(), tagsToRemove.size()),
        };
      case rt::TrackAuthoringStatus::NoOp: return TagEditResult{};
      case rt::TrackAuthoringStatus::Stale:
      case rt::TrackAuthoringStatus::Unavailable:
        return TagEditResult{
          .status = replyRes->status,
          .notificationText = "Library changed while the tag editor was open. Reload and try again.",
        };
    }

    gsl_Assert(false && "Tag edit returned an unknown authoring status");
    std::unreachable();
  }
} // namespace ao::uimodel
