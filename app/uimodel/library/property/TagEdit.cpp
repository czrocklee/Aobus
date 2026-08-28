// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TagEdit.h>

#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/uimodel/presentation/PresentationText.h>

#include <cstddef>
#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    async::Task<Result<TagEditResult>> finishTagEditAsync(async::Task<Result<TrackTagSubmitResult>> submission,
                                                          i18n::MessageCatalog textCatalog,
                                                          std::size_t const addedCount,
                                                          std::size_t const removedCount)
    {
      auto replyRes = co_await std::move(submission);

      if (!replyRes)
      {
        co_return std::unexpected{replyRes.error()};
      }

      switch (replyRes->status)
      {
        case rt::AuthoringStatus::Applied:
          co_return TagEditResult{
            .status = replyRes->status,
            .notificationText = i18n::requiredFormat(
              textCatalog,
              i18n::MessageId::TrackTagsChanged,
              {{"hasAdded", addedCount == 0 ? std::string_view{"no"} : std::string_view{"yes"}},
               {"hasRemoved", removedCount == 0 ? std::string_view{"no"} : std::string_view{"yes"}},
               {"addedCount", addedCount},
               {"removedCount", removedCount},
               {"trackCount", replyRes->reply.changes.size()}}),
          };
        case rt::AuthoringStatus::NoOp: co_return TagEditResult{.status = replyRes->status, .notificationText = {}};
        case rt::AuthoringStatus::Busy:
          co_return TagEditResult{
            .status = replyRes->status,
            .notificationText = std::string{i18n::requiredText(textCatalog, i18n::MessageId::LibraryBusyTryAgain)},
          };
        case rt::AuthoringStatus::Stale:
        case rt::AuthoringStatus::Unavailable:
          co_return TagEditResult{
            .status = replyRes->status,
            .notificationText = std::string{i18n::requiredText(textCatalog, i18n::MessageId::TrackTagsStale)},
          };
      }

      AO_FATAL("Tag edit returned an unknown authoring status");
    }
  } // namespace

  async::Task<Result<TagEditResult>> applyTagEdit(TrackAuthoringSession& session,
                                                  i18n::MessageCatalog const& textCatalog,
                                                  std::vector<std::string> tagsToAdd,
                                                  std::vector<std::string> tagsToRemove)
  {
    if (tagsToAdd.empty() && tagsToRemove.empty())
    {
      return async::makeReadyTask(Result<TagEditResult>{});
    }

    auto const addedCount = tagsToAdd.size();
    auto const removedCount = tagsToRemove.size();
    auto submission = session.submitTags(std::move(tagsToAdd), std::move(tagsToRemove));
    return finishTagEditAsync(std::move(submission), textCatalog, addedCount, removedCount);
  }
} // namespace ao::uimodel
