// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TagEdit.h>

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <expected>
#include <span>
#include <string>

namespace ao::uimodel
{
  Result<TagEditResult> applyTagEdit(TrackAuthoringSession& session,
                                     PresentationTextCatalog const& textCatalog,
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
            textCatalog.format(i18n::MessageId::TrackTagsChanged,
                               {{"hasAdded", tagsToAdd.empty() ? std::string_view{"no"} : std::string_view{"yes"}},
                                {"hasRemoved", tagsToRemove.empty() ? std::string_view{"no"} : std::string_view{"yes"}},
                                {"addedCount", tagsToAdd.size()},
                                {"removedCount", tagsToRemove.size()},
                                {"trackCount", replyRes->reply.changes.size()}}),
        };
      case rt::TrackAuthoringStatus::NoOp: return TagEditResult{};
      case rt::TrackAuthoringStatus::Stale:
      case rt::TrackAuthoringStatus::Unavailable:
        return TagEditResult{
          .status = replyRes->status,
          .notificationText = std::string{textCatalog.text(i18n::MessageId::TrackTagsStale)},
        };
    }

    AO_FATAL("Tag edit returned an unknown authoring status");
  }
} // namespace ao::uimodel
