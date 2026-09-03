// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/LibraryCommandsInternal.h"
#include "runtime/library/LibraryWriteLane.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/library/ListStore.h>
#include <ao/library/ListView.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/query/Field.h>
#include <ao/query/Parser.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompilation.h>
#include <ao/query/detail/Bytecode.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/WritableTagList.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>

#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    // Membership requests name a set of Tracks. A repeated id would otherwise
    // reach the reply, where targetTrackIds is what the CLI prints and what the
    // frontends count as "tracks affected"; the tag patch itself is already
    // idempotent per Track. Keeps first-seen order so the reply still reads back
    // in request order.
    std::vector<TrackId> distinctTrackIds(std::span<TrackId const> const trackIds)
    {
      auto seen = std::unordered_set<TrackId>{};
      seen.reserve(trackIds.size());
      auto distinct = std::vector<TrackId>{};
      distinct.reserve(trackIds.size());

      for (auto const trackId : trackIds)
      {
        if (seen.insert(trackId).second)
        {
          distinct.push_back(trackId);
        }
      }

      return distinct;
    }

    Result<std::string> requireWritableListTag(library::ListView const& view, ListId const listId)
    {
      auto optTag = writableTagForListExpression(view.filter());

      if (!optTag)
      {
        return makeError(
          Error::Code::InvalidInput,
          std::format("List {} membership is computed; its expression must be exactly one positive tag", listId));
      }

      return std::move(*optTag);
    }

    struct ParentFilterPlan final
    {
      ListId listId = kInvalidListId;
      query::ExecutionPlan plan{};
    };

    Error storedParentFilterError(ListId const parentId, Error error)
    {
      error.code = Error::Code::FormatRejected;
      error.message = std::format("invalid stored filter for parent List {}: {}", parentId, error.message);
      return error;
    }

    Result<std::vector<ParentFilterPlan>> compileParentFilterPlans(library::MusicLibrary& library,
                                                                   library::LibraryWrite& transaction,
                                                                   library::ListView const& targetView)
    {
      auto plans = std::vector<ParentFilterPlan>{};
      auto reader = library.lists().reader(transaction);
      auto parentId = targetView.parentId();
      auto visited = std::unordered_set<ListId>{};

      while (parentId != kInvalidListId)
      {
        auto const inserted = visited.insert(parentId).second;
        AO_INVARIANT(inserted, "List parent cycle detected while validating membership");

        auto optParent = reader.get(parentId);

        AO_INVARIANT(optParent, "List parent is missing while validating membership");

        if (!optParent->filter().empty())
        {
          auto expressionRes = query::parse(optParent->filter());

          if (!expressionRes)
          {
            return std::unexpected{storedParentFilterError(parentId, std::move(expressionRes).error())};
          }

          auto planRes = query::compileQuery(*expressionRes);

          if (!planRes)
          {
            return std::unexpected{storedParentFilterError(parentId, std::move(planRes).error())};
          }

          plans.push_back(ParentFilterPlan{.listId = parentId, .plan = std::move(*planRes)});
        }

        parentId = optParent->parentId();
      }

      return plans;
    }

    Result<> validateTracksExist(library::MusicLibrary& library,
                                 library::LibraryWrite& transaction,
                                 std::span<TrackId const> trackIds)
    {
      auto reader = library.tracks().reader(transaction);

      for (auto const trackId : trackIds)
      {
        if (!reader.get(trackId, library::TrackStore::Reader::LoadMode::Hot))
        {
          return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
        }
      }

      return {};
    }

    Result<> validateParentMembership(library::MusicLibrary& library,
                                      library::LibraryWrite& transaction,
                                      library::ListView const& targetView,
                                      ListId const targetListId,
                                      std::span<TrackId const> trackIds)
    {
      auto plansRes = compileParentFilterPlans(library, transaction, targetView);

      if (!plansRes)
      {
        return std::unexpected{plansRes.error()};
      }

      auto& plans = *plansRes;

      if (plans.empty())
      {
        return validateTracksExist(library, transaction, trackIds);
      }

      auto dictionaryCache = library::DictionaryReadCache{library.dictionary()};
      auto dictionaryContext = library::DictionaryReadContext{dictionaryCache};
      auto bindings = std::vector<query::PlanBinding>{};
      bindings.reserve(plans.size());

      for (auto const& plan : plans)
      {
        bindings.emplace_back(plan.plan, dictionaryContext);
      }

      auto reader = library.tracks().reader(transaction);
      auto evaluator = query::PlanEvaluator{};

      for (auto const trackId : trackIds)
      {
        auto optTrack = reader.get(trackId, library::TrackStore::Reader::LoadMode::Both);

        if (!optTrack)
        {
          return makeError(Error::Code::NotFound, std::format("track not found: {}", trackId));
        }

        for (std::size_t index = 0; index < plans.size(); ++index)
        {
          AO_INVARIANT(query::hasRequiredTrackData(plans[index].plan.accessProfile, *optTrack),
                       "Complete Track {} lacks data required by parent List {}",
                       trackId,
                       plans[index].listId);

          if (!evaluator.matches(bindings[index], *optTrack))
          {
            return makeError(
              Error::Code::InvalidInput,
              std::format(
                "Track {} is outside parent List {} of target List {}", trackId, plans[index].listId, targetListId));
          }
        }
      }

      return {};
    }

    Result<AddTracksToListReply> applyAddTracksToListInTransaction(library::MusicLibrary& library,
                                                                   library::LibraryWrite& transaction,
                                                                   ListId const listId,
                                                                   std::span<TrackId const> trackIds)
    {
      auto targets = distinctTrackIds(trackIds);
      auto listName = std::string{};
      auto tag = std::string{};

      // LMDB values are borrowed from the transaction and may be invalidated by
      // a later write. Materialize every List field used by the reply before
      // detail::applyTagPatchInTransaction performs any database mutation.
      {
        auto listWriter = transaction.lists();
        auto viewRes = detail::requireList(listWriter, listId);

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto tagRes = requireWritableListTag(*viewRes, listId);

        if (!tagRes)
        {
          return std::unexpected{tagRes.error()};
        }

        if (auto eligibilityRes = validateParentMembership(library, transaction, *viewRes, listId, targets);
            !eligibilityRes)
        {
          return std::unexpected{eligibilityRes.error()};
        }

        listName = std::string{viewRes->name()};
        tag = std::move(*tagRes);
      }

      auto tags = std::array<std::string, 1>{tag};
      auto editRes = detail::applyTagPatchInTransaction(library, transaction, targets, tags, {});

      if (!editRes)
      {
        return std::unexpected{editRes.error()};
      }

      return AddTracksToListReply{
        .listId = listId,
        .listName = std::move(listName),
        .tag = std::move(tag),
        .targetTrackIds = std::move(targets),
        .tagEdit = std::move(*editRes),
      };
    }

    struct RemoveTracksFromListWork final
    {
      RemoveTracksFromListReply reply{};
      std::optional<ListOrderChange> optOrderChange{};
    };

    Result<RemoveTracksFromListWork> applyRemoveTracksFromListInTransaction(library::MusicLibrary& library,
                                                                            library::LibraryWrite& transaction,
                                                                            ListId const listId,
                                                                            std::span<TrackId const> trackIds)
    {
      auto targets = distinctTrackIds(trackIds);

      if (auto existenceRes = validateTracksExist(library, transaction, targets); !existenceRes)
      {
        return std::unexpected{existenceRes.error()};
      }

      auto const selected = std::unordered_set<TrackId>{targets.begin(), targets.end()};
      auto listName = std::string{};
      auto tag = std::string{};
      auto oldOrder = std::vector<TrackId>{};
      auto nextOrder = std::vector<TrackId>{};
      auto forgotten = std::vector<TrackId>{};
      auto optList = std::optional<library::ListBuilder>{};

      {
        auto listWriter = transaction.lists();
        auto viewRes = detail::requireList(listWriter, listId);

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto tagRes = requireWritableListTag(*viewRes, listId);

        if (!tagRes)
        {
          return std::unexpected{tagRes.error()};
        }

        listName = std::string{viewRes->name()};
        tag = std::move(*tagRes);
        oldOrder = detail::orderTrackIdsFrom(*viewRes);
        nextOrder = oldOrder;

        for (auto const trackId : oldOrder)
        {
          if (selected.contains(trackId))
          {
            forgotten.push_back(trackId);
          }
        }

        std::erase_if(nextOrder, [&selected](TrackId const trackId) { return selected.contains(trackId); });

        if (nextOrder != oldOrder)
        {
          optList.emplace(detail::listWithOrder(*viewRes, nextOrder));
        }
      }

      auto tags = std::array<std::string, 1>{tag};
      auto editRes = detail::applyTagPatchInTransaction(library, transaction, targets, {}, tags);

      if (!editRes)
      {
        return std::unexpected{editRes.error()};
      }

      auto work = RemoveTracksFromListWork{
        .reply =
          RemoveTracksFromListReply{
            .listId = listId,
            .listName = std::move(listName),
            .tag = std::move(tag),
            .targetTrackIds = std::move(targets),
            .tagEdit = std::move(*editRes),
            .forgottenPositionTrackIds = std::move(forgotten),
          },
      };

      if (nextOrder == oldOrder)
      {
        return work;
      }

      AO_INVARIANT(optList, "Changed List order did not retain its semantic update");

      if (auto updateRes = transaction.lists().update(listId, *optList); !updateRes)
      {
        return detail::storageError("Failed to forget removed List positions", updateRes.error());
      }

      work.optOrderChange = ListOrderChange{
        .listId = listId,
        .operation = detail::removalScriptFor(oldOrder, selected),
      };
      return work;
    }
  } // namespace

  async::Task<Result<AddTracksToListReply>> LibraryCommands::Impl::previewAddTracksToList(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    std::vector<TrackId> trackIds)
  {
    return detail::applyInteractivePreviewAsync(
      std::move(submission),
      [this, listId, trackIds = std::move(trackIds)](library::LibraryWrite& transaction)
      { return applyAddTracksToListInTransaction(library, transaction, listId, trackIds); });
  }

  async::Task<Result<TrackAuthoringResult<AddTracksToListReply>>> LibraryCommands::Impl::applyAddTracksToList(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    BoundTrackTargets targets)
  {
    return detail::executeBoundTrackAuthoringAsync<AddTracksToListReply>(
      std::move(submission),
      std::move(targets),
      "Add tracks to list",
      [this, listId](library::LibraryWrite& transaction,
                     std::span<TrackId const> trackIds) -> Result<OperationOutcome<AddTracksToListReply>>
      {
        auto replyRes = applyAddTracksToListInTransaction(library, transaction, listId, trackIds);

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);

        if (reply.tagEdit.changes.empty())
        {
          return Unchanged<AddTracksToListReply>{.value = std::move(reply)};
        }

        auto mutatedIds =
          reply.tagEdit.changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
        return Changed<AddTracksToListReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)},
        };
      });
  }

  async::Task<Result<RemoveTracksFromListReply>> LibraryCommands::Impl::previewRemoveTracksFromList(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    std::vector<TrackId> trackIds)
  {
    auto workRes = co_await detail::applyInteractivePreviewAsync(
      std::move(submission),
      [this, listId, trackIds = std::move(trackIds)](library::LibraryWrite& transaction)
      { return applyRemoveTracksFromListInTransaction(library, transaction, listId, trackIds); });

    if (!workRes)
    {
      co_return std::unexpected{workRes.error()};
    }

    co_return std::move(workRes->reply);
  }

  async::Task<Result<TrackAuthoringResult<RemoveTracksFromListReply>>> LibraryCommands::Impl::applyRemoveTracksFromList(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    BoundTrackTargets targets)
  {
    return detail::executeBoundTrackAuthoringAsync<RemoveTracksFromListReply>(
      std::move(submission),
      std::move(targets),
      "Remove tracks from list",
      [this, listId](library::LibraryWrite& transaction,
                     std::span<TrackId const> trackIds) -> Result<OperationOutcome<RemoveTracksFromListReply>>
      {
        auto workRes = applyRemoveTracksFromListInTransaction(library, transaction, listId, trackIds);

        if (!workRes)
        {
          return std::unexpected{workRes.error()};
        }

        auto work = std::move(*workRes);
        auto mutatedIds = work.reply.tagEdit.changes | std::views::transform(&TrackTagsChange::trackId) |
                          std::ranges::to<std::vector>();

        if (auto const orderChanged = work.optOrderChange.has_value(); mutatedIds.empty() && !orderChanged)
        {
          return Unchanged<RemoveTracksFromListReply>{.value = std::move(work.reply)};
        }

        auto changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedIds)};

        if (work.optOrderChange)
        {
          changeSet.listsUpserted.push_back(listId);
          changeSet.listOrderChanges.push_back(std::move(*work.optOrderChange));
        }

        return Changed<RemoveTracksFromListReply>{
          .value = std::move(work.reply),
          .changeSet = std::move(changeSet),
        };
      });
  }
} // namespace ao::rt
