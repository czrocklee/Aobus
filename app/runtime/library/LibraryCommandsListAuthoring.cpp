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
#include <ao/query/Parser.h>
#include <ao/query/QueryCompilation.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/WritableTagList.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    Result<ListDraft> normalizeListDraft(ListDraft const& draft)
    {
      auto normalized = draft;
      auto nameRes = detail::normalizeRuntimeText(draft.name, "List name");

      if (!nameRes)
      {
        return std::unexpected{nameRes.error()};
      }

      auto descriptionRes = detail::normalizeRuntimeText(draft.description, "List description");

      if (!descriptionRes)
      {
        return std::unexpected{descriptionRes.error()};
      }

      normalized.name = std::move(*nameRes);
      normalized.description = std::move(*descriptionRes);
      return normalized;
    }

    std::unexpected<Error> prefixError(char const* prefix, Error const& error)
    {
      return std::unexpected{Error{
        .code = error.code,
        .message = std::format("{}: {}", prefix, error.message),
        .location = error.location,
      }};
    }

    template<typename Reply, typename Operation>
    async::Task<Result<AuthoringResult<Reply>>> executeBoundListOrderAuthoringAsync(
      LibraryWriteLane::Submission submission,
      BoundListOrder order,
      std::string operationName,
      Operation operation)
    {
      auto start = co_await LibraryWriteLane::beginListOrderAuthoringMutationAsync(std::move(submission), order);
      auto result = AuthoringResult<Reply>{.status = start.status};

      if (!start.optMutation)
      {
        co_return result;
      }

      auto executionRes = co_await start.optMutation->executeAsync(
        [&order, operation = std::move(operation)](
          library::LibraryWrite& transaction) mutable -> Result<OperationOutcome<Reply>>
        { return operation(transaction, order); },
        std::move(operationName));

      if (!executionRes)
      {
        co_return std::unexpected{executionRes.error()};
      }

      result.reply = std::move(executionRes->value);
      result.status = executionRes->optCommittedRevision ? AuthoringStatus::Applied : AuthoringStatus::NoOp;
      co_return result;
    }

    Result<> validateListExpression(std::string const& expression)
    {
      auto exprRes = query::parse(expression.empty() ? "true" : expression);

      if (!exprRes)
      {
        return prefixError("invalid list filter", exprRes.error());
      }

      if (auto planRes = query::compileQuery(*exprRes); !planRes)
      {
        return prefixError("invalid list filter", planRes.error());
      }

      return {};
    }

    library::ListBuilder listForDraft(ListDraft const& draft,
                                      std::optional<library::ListView> const& optExisting = std::nullopt)
    {
      auto builder = optExisting ? library::ListBuilder::fromView(*optExisting) : library::ListBuilder::makeEmpty();
      builder.name(draft.name).description(draft.description).filter(draft.expression).parentId(draft.parentId);
      return builder;
    }

    Result<> validateListDraft(ListDraft const& draft)
    {
      return validateListExpression(draft.expression);
    }

    void appendListFieldChange(std::vector<ListFieldChange>& changes,
                               std::string_view field,
                               std::string_view oldValue,
                               std::string_view newValue)
    {
      if (oldValue == newValue)
      {
        return;
      }

      changes.push_back(ListFieldChange{
        .field = std::string{field}, .oldValue = std::string{oldValue}, .newValue = std::string{newValue}});
    }

    UpdateListReply diffListUpdate(library::ListView const& existing, ListDraft const& draft)
    {
      auto reply = UpdateListReply{};
      appendListFieldChange(reply.fieldChanges, "name", existing.name(), draft.name);
      appendListFieldChange(reply.fieldChanges, "description", existing.description(), draft.description);
      appendListFieldChange(reply.fieldChanges,
                            "parentId",
                            std::format("{}", existing.parentId().raw()),
                            std::format("{}", draft.parentId.raw()));
      appendListFieldChange(reply.fieldChanges, "filter", existing.filter(), draft.expression);

      return reply;
    }

    Result<ListId> createListInTransaction(library::LibraryWrite& transaction, ListDraft const& draft)
    {
      auto normalizedDraftRes = normalizeListDraft(draft);

      if (!normalizedDraftRes)
      {
        return std::unexpected{normalizedDraftRes.error()};
      }

      auto const& normalizedDraft = *normalizedDraftRes;
      auto listWriter = transaction.lists();
      auto list = listForDraft(normalizedDraft);

      if (auto result = validateListDraft(normalizedDraft); !result)
      {
        return std::unexpected{result.error()};
      }

      auto result = listWriter.create(list);

      if (!result)
      {
        return detail::storageError("Failed to create list", result.error());
      }

      return *result;
    }

    Result<UpdateListReply> updateListInTransaction(library::LibraryWrite& transaction, ListDraft const& draft)
    {
      auto normalizedDraftRes = normalizeListDraft(draft);

      if (!normalizedDraftRes)
      {
        return std::unexpected{normalizedDraftRes.error()};
      }

      auto const& normalizedDraft = *normalizedDraftRes;
      auto listWriter = transaction.lists();
      auto optExisting = listWriter.get(normalizedDraft.listId);

      if (!optExisting)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", normalizedDraft.listId));
      }

      auto list = listForDraft(normalizedDraft, optExisting);

      if (auto result = validateListDraft(normalizedDraft); !result)
      {
        return std::unexpected{result.error()};
      }

      auto reply = diffListUpdate(*optExisting, normalizedDraft);

      if (reply.fieldChanges.empty())
      {
        return reply;
      }

      reply.changed = true;

      if (auto result = listWriter.update(normalizedDraft.listId, list); !result)
      {
        return detail::storageError("Failed to update list", result.error());
      }

      return reply;
    }

    struct DeleteListTagImpactWork final
    {
      DeleteListReply::TagImpact impact{};
      std::vector<TrackId> taggedTrackIds{};
    };

    Result<std::optional<DeleteListTagImpactWork>> analyzeDeleteListTagImpact(
      library::MusicLibrary& library,
      library::LibraryWrite& transaction,
      library::ListView const& targetView,
      std::span<ListId const> const deletedListIds)
    {
      auto const optTag = writableTagForListExpression(targetView.filter());

      if (!optTag)
      {
        return std::optional<DeleteListTagImpactWork>{};
      }

      auto work = DeleteListTagImpactWork{};
      work.impact.tag = *optTag;
      auto const deleted = std::unordered_set<ListId>{deletedListIds.begin(), deletedListIds.end()};

      for (auto const& [listId, view] : library.lists().reader(transaction))
      {
        if (!deleted.contains(listId) && listExpressionReferencesTag(view.filter(), *optTag))
        {
          work.impact.otherListReferences.push_back(
            DeleteListReply::TagReference{.listId = listId, .name = std::string{view.name()}});
        }
      }

      auto const& dictionary = library.dictionary();

      for (auto const& [trackId, view] : library.tracks().reader(transaction).hot())
      {
        bool hasTag = false;

        for (auto const tagId : view.tags())
        {
          if (dictionary.get(tagId) == *optTag)
          {
            hasTag = true;
            break;
          }
        }

        if (hasTag)
        {
          work.taggedTrackIds.push_back(trackId);
        }
      }

      work.impact.taggedTrackCount = work.taggedTrackIds.size();
      return std::optional<DeleteListTagImpactWork>{std::move(work)};
    }

    Result<std::vector<DeleteListReply>> collectDeleteListSubtree(library::MusicLibrary& library,
                                                                  library::LibraryWrite& transaction,
                                                                  ListId const rootListId)
    {
      auto records = std::unordered_map<ListId, DeleteListReply>{};
      auto childIdsByParent = std::unordered_map<ListId, std::vector<ListId>>{};

      for (auto const& [savedListId, view] : library.lists().reader(transaction))
      {
        records.emplace(savedListId,
                        DeleteListReply{
                          .listId = savedListId,
                          .name = std::string{view.name()},
                          .orderTrackIdCount = view.orderTrackIds().size(),
                        });
        childIdsByParent[view.parentId()].push_back(savedListId);
      }

      if (!records.contains(rootListId))
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", rootListId));
      }

      auto deletedLists = std::vector<DeleteListReply>{};
      auto visiting = std::unordered_set<ListId>{};
      auto visited = std::unordered_set<ListId>{};
      auto visit = std::function<Result<>(ListId)>{};
      visit = [&](ListId const currentId) -> Result<>
      {
        if (visited.contains(currentId))
        {
          return {};
        }

        if (!visiting.insert(currentId).second)
        {
          return makeError(Error::Code::InvalidState, "List cycle detected while previewing subtree deletion");
        }

        auto const record = records.find(currentId);

        if (record == records.end())
        {
          return makeError(Error::Code::InvalidState, "List disappeared while previewing subtree deletion");
        }

        deletedLists.push_back(record->second);

        if (auto const children = childIdsByParent.find(currentId); children != childIdsByParent.end())
        {
          for (auto const childId : children->second)
          {
            if (auto result = visit(childId); !result)
            {
              return result;
            }
          }
        }

        visiting.erase(currentId);
        visited.insert(currentId);
        return {};
      };

      if (auto result = visit(rootListId); !result)
      {
        return std::unexpected{result.error()};
      }

      return deletedLists;
    }

    Result<LibraryChangeSet> applyMoveListOrderInTransaction(library::LibraryWrite& transaction,
                                                             BoundListOrder const& order,
                                                             std::span<TrackId const> selectedTrackIds,
                                                             std::span<TrackId const> desiredEffectiveTrackIds,
                                                             std::optional<TrackId> const optBeforeTrackId)
    {
      auto listWriter = transaction.lists();
      auto viewRes = detail::requireList(listWriter, order.listId());

      if (!viewRes)
      {
        return std::unexpected{viewRes.error()};
      }

      auto const& view = *viewRes;
      auto const oldOrderTrackIds = detail::orderTrackIdsFrom(view);
      auto nextOrderTrackIds = oldOrderTrackIds;
      auto rankedMembership = std::unordered_set<TrackId>{nextOrderTrackIds.begin(), nextOrderTrackIds.end()};
      auto const effectiveTrackIds = order.effectiveTrackIds();
      auto const effectiveMembership = std::unordered_set<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()};
      auto const selectedMembership = std::unordered_set<TrackId>{selectedTrackIds.begin(), selectedTrackIds.end()};

      for (auto const trackId : effectiveTrackIds)
      {
        if (rankedMembership.insert(trackId).second)
        {
          nextOrderTrackIds.push_back(trackId);
        }
      }

      std::erase_if(nextOrderTrackIds,
                    [&selectedMembership](TrackId const trackId) { return selectedMembership.contains(trackId); });
      auto orderInsertion = nextOrderTrackIds.end();

      if (optBeforeTrackId)
      {
        orderInsertion = std::ranges::find(nextOrderTrackIds, *optBeforeTrackId);

        AO_INVARIANT(
          orderInsertion != nextOrderTrackIds.end(), "Bound List order anchor is absent from materialized order");
      }

      nextOrderTrackIds.insert(orderInsertion, selectedTrackIds.begin(), selectedTrackIds.end());

      auto projectedOrder = std::vector<TrackId>{};
      projectedOrder.reserve(effectiveTrackIds.size());

      for (auto const trackId : nextOrderTrackIds)
      {
        if (effectiveMembership.contains(trackId))
        {
          projectedOrder.push_back(trackId);
        }
      }

      AO_INVARIANT(std::ranges::equal(projectedOrder, desiredEffectiveTrackIds),
                   "Materialized List order does not represent the requested move");

      auto list = detail::listWithOrder(view, nextOrderTrackIds);

      if (auto updateRes = listWriter.update(order.listId(), list); !updateRes)
      {
        return detail::storageError("Failed to update List order", updateRes.error());
      }

      auto script = delta::diff(oldOrderTrackIds, nextOrderTrackIds, {}, selectedTrackIds);
      return LibraryChangeSet{.listsUpserted = {order.listId()},
                              .listOrderChanges = {
                                ListOrderChange{.listId = order.listId(), .operation = std::move(script)},
                              }};
    }

    Result<detail::ChangedWork<DeleteListReply>> applyDeleteListInTransaction(library::MusicLibrary& library,
                                                                              library::LibraryWrite& transaction,
                                                                              ListId const listId,
                                                                              DeleteListOptions const options)
    {
      auto listWriter = transaction.lists();
      auto optView = listWriter.get(listId);

      if (!optView)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
      }

      auto dependentDescriptions = std::string{};

      for (auto const& [dependentId, dependentView] : library.lists().reader(transaction))
      {
        if (dependentView.parentId() == listId)
        {
          if (!dependentDescriptions.empty())
          {
            dependentDescriptions.append(", ");
          }

          dependentDescriptions.append(std::format("{} ({})", dependentView.name(), dependentId));
        }
      }

      if (!dependentDescriptions.empty())
      {
        return makeError(
          Error::Code::Conflict, std::format("List {} has dependent Lists: {}", listId, dependentDescriptions));
      }

      auto work = detail::ChangedWork<DeleteListReply>{
        .reply =
          DeleteListReply{
            .listId = listId,
            .name = std::string{optView->name()},
            .orderTrackIdCount = optView->orderTrackIds().size(),
          },
      };
      auto const deletedListIds = std::array{listId};
      auto tagImpactWorkRes = analyzeDeleteListTagImpact(library, transaction, *optView, deletedListIds);

      if (!tagImpactWorkRes)
      {
        return std::unexpected{tagImpactWorkRes.error()};
      }

      auto optTagImpactWork = std::move(*tagImpactWorkRes);

      if (auto removeRes = listWriter.remove(listId); !removeRes)
      {
        return std::unexpected{removeRes.error()};
      }

      auto mutatedTrackIds = std::vector<TrackId>{};

      if (options.removeWritableTagFromTracks)
      {
        if (!optTagImpactWork)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("List {} does not have directly editable tag membership", listId));
        }

        auto& tagImpactWork = *optTagImpactWork;
        auto const tags = std::array{tagImpactWork.impact.tag};
        auto tagEditRes =
          detail::applyTagPatchInTransaction(library, transaction, tagImpactWork.taggedTrackIds, {}, tags);

        if (!tagEditRes)
        {
          return std::unexpected{tagEditRes.error()};
        }

        tagImpactWork.impact.removedFromTrackCount = tagEditRes->changes.size();
        mutatedTrackIds =
          tagEditRes->changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
      }

      if (optTagImpactWork)
      {
        work.reply.optTagImpact = optTagImpactWork->impact;
      }

      work.changeSet = LibraryChangeSet{.tracksMutated = std::move(mutatedTrackIds), .listsDeleted = {listId}};
      return work;
    }

    Result<detail::ChangedWork<DeleteListSubtreeReply>> applyDeleteListSubtreeInTransaction(
      library::MusicLibrary& library,
      library::LibraryWrite& transaction,
      ListId const listId,
      DeleteListOptions const options)
    {
      auto deletedListsRes = collectDeleteListSubtree(library, transaction, listId);

      if (!deletedListsRes)
      {
        return std::unexpected{deletedListsRes.error()};
      }

      auto work = detail::ChangedWork<DeleteListSubtreeReply>{
        .reply = DeleteListSubtreeReply{.rootListId = listId, .deletedLists = std::move(*deletedListsRes)},
      };
      auto deletedIds =
        work.reply.deletedLists | std::views::transform(&DeleteListReply::listId) | std::ranges::to<std::vector>();
      auto optRootView = library.lists().reader(transaction).get(listId);

      if (!optRootView)
      {
        return makeError(Error::Code::NotFound, std::format("list not found: {}", listId));
      }

      auto tagImpactWorkRes = analyzeDeleteListTagImpact(library, transaction, *optRootView, deletedIds);

      if (!tagImpactWorkRes)
      {
        return std::unexpected{tagImpactWorkRes.error()};
      }

      auto optTagImpactWork = std::move(*tagImpactWorkRes);
      auto listWriter = transaction.lists();
      auto removedIdsRes = listWriter.removeSubtree(listId);

      if (!removedIdsRes)
      {
        return std::unexpected{removedIdsRes.error()};
      }

      AO_INVARIANT(std::ranges::is_permutation(*removedIdsRes, deletedIds),
                   "Logical List subtree membership disagreed with the application snapshot");

      auto mutatedTrackIds = std::vector<TrackId>{};

      if (options.removeWritableTagFromTracks)
      {
        if (!optTagImpactWork)
        {
          return makeError(
            Error::Code::InvalidInput, std::format("List {} does not have directly editable tag membership", listId));
        }

        auto& tagImpactWork = *optTagImpactWork;
        auto const tags = std::array{tagImpactWork.impact.tag};
        auto tagEditRes =
          detail::applyTagPatchInTransaction(library, transaction, tagImpactWork.taggedTrackIds, {}, tags);

        if (!tagEditRes)
        {
          return std::unexpected{tagEditRes.error()};
        }

        tagImpactWork.impact.removedFromTrackCount = tagEditRes->changes.size();
        mutatedTrackIds =
          tagEditRes->changes | std::views::transform(&TrackTagsChange::trackId) | std::ranges::to<std::vector>();
      }

      if (optTagImpactWork)
      {
        work.reply.deletedLists.front().optTagImpact = optTagImpactWork->impact;
      }

      work.changeSet =
        LibraryChangeSet{.tracksMutated = std::move(mutatedTrackIds), .listsDeleted = std::move(deletedIds)};
      return work;
    }
  } // namespace

  async::Task<Result<ListId>> LibraryCommands::Impl::createList(LibraryWriteLane::Submission submission,
                                                                ListDraft draft)
  {
    auto executionRes = co_await detail::executeInteractiveMutationAsync(
      std::move(submission),
      "Create list",
      [draft = std::move(draft)](library::LibraryWrite& transaction) -> Result<OperationOutcome<ListId>>
      {
        auto listIdRes = createListInTransaction(transaction, draft);

        if (!listIdRes)
        {
          return std::unexpected{listIdRes.error()};
        }

        auto const listId = *listIdRes;
        return Changed<ListId>{
          .value = listId,
          .changeSet = LibraryChangeSet{.listsUpserted = {listId}},
        };
      });

    if (!executionRes)
    {
      co_return std::unexpected{executionRes.error()};
    }

    AO_INVARIANT(executionRes->optCommittedRevision, "List creation did not commit its generated identity");
    co_return executionRes->value;
  }

  async::Task<Result<>> LibraryCommands::Impl::previewCreateList(LibraryWriteLane::Submission submission,
                                                                 ListDraft draft)
  {
    auto listIdRes =
      co_await detail::applyInteractivePreviewAsync(std::move(submission),
                                                    [draft = std::move(draft)](library::LibraryWrite& transaction)
                                                    { return createListInTransaction(transaction, draft); });

    if (!listIdRes)
    {
      co_return std::unexpected{listIdRes.error()};
    }

    co_return Result<>{};
  }

  async::Task<Result<UpdateListReply>> LibraryCommands::Impl::updateList(LibraryWriteLane::Submission submission,
                                                                         ListDraft draft)
  {
    auto executionRes = co_await detail::executeInteractiveMutationAsync(
      std::move(submission),
      "Update list",
      [draft = std::move(draft)](library::LibraryWrite& transaction) -> Result<OperationOutcome<UpdateListReply>>
      {
        auto replyRes = updateListInTransaction(transaction, draft);

        if (!replyRes)
        {
          return std::unexpected{replyRes.error()};
        }

        auto reply = std::move(*replyRes);

        if (!reply.changed)
        {
          return Unchanged<UpdateListReply>{.value = std::move(reply)};
        }

        return Changed<UpdateListReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.listsUpserted = {draft.listId}},
        };
      });

    if (!executionRes)
    {
      co_return std::unexpected{executionRes.error()};
    }

    co_return std::move(executionRes->value);
  }

  async::Task<Result<UpdateListReply>> LibraryCommands::Impl::previewUpdateList(LibraryWriteLane::Submission submission,
                                                                                ListDraft draft)
  {
    return detail::applyInteractivePreviewAsync(std::move(submission),
                                                [draft = std::move(draft)](library::LibraryWrite& transaction)
                                                { return updateListInTransaction(transaction, draft); });
  }

  async::Task<Result<AuthoringResult<MoveListOrderReply>>> LibraryCommands::Impl::applyMoveListOrder(
    LibraryWriteLane::Submission submission,
    BoundListOrder order,
    std::vector<TrackId> selectedTrackIds,
    std::optional<TrackId> const optBeforeTrackId)
  {
    auto start = co_await LibraryWriteLane::beginListOrderAuthoringMutationAsync(std::move(submission), order);
    auto result = AuthoringResult<MoveListOrderReply>{.status = start.status};

    if (!start.optMutation)
    {
      co_return result;
    }

    auto const effectiveTrackIds = order.effectiveTrackIds();
    auto const effectiveMembership = std::unordered_set<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()};
    auto selectedMembership = std::unordered_set<TrackId>{};

    for (auto const trackId : selectedTrackIds)
    {
      if (!effectiveMembership.contains(trackId))
      {
        co_return makeError(
          Error::Code::InvalidInput, std::format("List order selection is not in the bound source: {}", trackId));
      }

      selectedMembership.insert(trackId);
    }

    for (auto const trackId : effectiveTrackIds)
    {
      if (selectedMembership.contains(trackId))
      {
        result.reply.selectedTrackIds.push_back(trackId);
      }
    }

    result.reply.optBeforeTrackId = optBeforeTrackId;

    if (result.reply.selectedTrackIds.empty())
    {
      result.status = AuthoringStatus::NoOp;
      co_return result;
    }

    if (optBeforeTrackId &&
        (!effectiveMembership.contains(*optBeforeTrackId) || selectedMembership.contains(*optBeforeTrackId)))
    {
      co_return makeError(Error::Code::InvalidInput, "List order anchor must be an unselected bound source track");
    }

    auto desiredEffectiveTrackIds = std::vector<TrackId>{};
    desiredEffectiveTrackIds.reserve(effectiveTrackIds.size());

    for (auto const trackId : effectiveTrackIds)
    {
      if (!selectedMembership.contains(trackId))
      {
        desiredEffectiveTrackIds.push_back(trackId);
      }
    }

    auto insertion = desiredEffectiveTrackIds.end();

    if (optBeforeTrackId)
    {
      insertion = std::ranges::find(desiredEffectiveTrackIds, *optBeforeTrackId);
    }

    desiredEffectiveTrackIds.insert(
      insertion, result.reply.selectedTrackIds.begin(), result.reply.selectedTrackIds.end());

    if (std::ranges::equal(effectiveTrackIds, desiredEffectiveTrackIds))
    {
      result.status = AuthoringStatus::NoOp;
      co_return result;
    }

    auto executionRes = co_await start.optMutation->executeAsync(
      [&order, &desiredEffectiveTrackIds, &result, optBeforeTrackId](
        library::LibraryWrite& transaction) -> Result<OperationOutcome<MoveListOrderReply>>
      {
        auto changeSetRes = applyMoveListOrderInTransaction(
          transaction, order, result.reply.selectedTrackIds, desiredEffectiveTrackIds, optBeforeTrackId);

        if (!changeSetRes)
        {
          return std::unexpected{changeSetRes.error()};
        }

        return Changed<MoveListOrderReply>{
          .value = std::move(result.reply),
          .changeSet = std::move(*changeSetRes),
        };
      },
      "Move list order");

    if (!executionRes)
    {
      co_return std::unexpected{executionRes.error()};
    }

    // Reset and Forget decide their no-op inside the transaction, so they read
    // the committed revision to separate Applied from NoOp. Move decides its
    // own above and its operation returns only Changed.
    AO_INVARIANT(executionRes->optCommittedRevision, "Move list order did not commit its new order");
    result.reply = std::move(executionRes->value);
    result.status = AuthoringStatus::Applied;
    co_return result;
  }

  async::Task<Result<AuthoringResult<ResetListOrderReply>>> LibraryCommands::Impl::applyResetListOrder(
    LibraryWriteLane::Submission submission,
    BoundListOrder order)
  {
    return executeBoundListOrderAuthoringAsync<ResetListOrderReply>(
      std::move(submission),
      std::move(order),
      "Reset list order",
      [](library::LibraryWrite& transaction,
         BoundListOrder const& order) -> Result<OperationOutcome<ResetListOrderReply>>
      {
        auto listWriter = transaction.lists();
        auto viewRes = detail::requireList(listWriter, order.listId());

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto const oldOrderTrackIds = detail::orderTrackIdsFrom(*viewRes);
        auto reply = ResetListOrderReply{.forgottenPositionCount = oldOrderTrackIds.size()};

        if (oldOrderTrackIds.empty())
        {
          return Unchanged<ResetListOrderReply>{.value = std::move(reply)};
        }

        auto list = detail::listWithOrder(*viewRes, {});

        if (auto updateRes = listWriter.update(order.listId(), list); !updateRes)
        {
          return detail::storageError("Failed to reset List order", updateRes.error());
        }

        return Changed<ResetListOrderReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.listsUpserted = {order.listId()},
                                        .listOrderChanges =
                                          {
                                            ListOrderChange{.listId = order.listId(), .operation = ListOrderReset{}},
                                          }},
        };
      });
  }

  async::Task<Result<AuthoringResult<ForgetHiddenListOrderReply>>> LibraryCommands::Impl::applyForgetHiddenListOrder(
    LibraryWriteLane::Submission submission,
    BoundListOrder order)
  {
    return executeBoundListOrderAuthoringAsync<ForgetHiddenListOrderReply>(
      std::move(submission),
      std::move(order),
      "Forget hidden list order",
      [](library::LibraryWrite& transaction,
         BoundListOrder const& order) -> Result<OperationOutcome<ForgetHiddenListOrderReply>>
      {
        auto listWriter = transaction.lists();
        auto viewRes = detail::requireList(listWriter, order.listId());

        if (!viewRes)
        {
          return std::unexpected{viewRes.error()};
        }

        auto const oldOrderTrackIds = detail::orderTrackIdsFrom(*viewRes);
        auto const effectiveTrackIds = order.effectiveTrackIds();
        auto const effectiveMembership =
          std::unordered_set<TrackId>{effectiveTrackIds.begin(), effectiveTrackIds.end()};
        auto nextOrderTrackIds = std::vector<TrackId>{};
        nextOrderTrackIds.reserve(oldOrderTrackIds.size());

        for (auto const trackId : oldOrderTrackIds)
        {
          if (effectiveMembership.contains(trackId))
          {
            nextOrderTrackIds.push_back(trackId);
          }
        }

        auto reply =
          ForgetHiddenListOrderReply{.forgottenPositionCount = oldOrderTrackIds.size() - nextOrderTrackIds.size()};

        if (reply.forgottenPositionCount == 0)
        {
          return Unchanged<ForgetHiddenListOrderReply>{.value = std::move(reply)};
        }

        auto list = detail::listWithOrder(*viewRes, nextOrderTrackIds);

        if (auto updateRes = listWriter.update(order.listId(), list); !updateRes)
        {
          return detail::storageError("Failed to forget hidden List positions", updateRes.error());
        }

        auto script = delta::diff(oldOrderTrackIds, nextOrderTrackIds);
        return Changed<ForgetHiddenListOrderReply>{
          .value = std::move(reply),
          .changeSet = LibraryChangeSet{.listsUpserted = {order.listId()},
                                        .listOrderChanges =
                                          {
                                            ListOrderChange{.listId = order.listId(), .operation = std::move(script)},
                                          }},
        };
      });
  }

  async::Task<Result<DeleteListReply>> LibraryCommands::Impl::deleteList(LibraryWriteLane::Submission submission,
                                                                         ListId const listId,
                                                                         DeleteListOptions const options)
  {
    return detail::executeChangedWorkAsync<DeleteListReply>(
      std::move(submission),
      "Delete list",
      [this, listId, options](library::LibraryWrite& transaction)
      { return applyDeleteListInTransaction(library, transaction, listId, options); });
  }

  async::Task<Result<DeleteListReply>> LibraryCommands::Impl::previewDeleteList(LibraryWriteLane::Submission submission,
                                                                                ListId const listId,
                                                                                DeleteListOptions const options)
  {
    return detail::previewChangedWorkAsync<DeleteListReply>(
      std::move(submission),
      [this, listId, options](library::LibraryWrite& transaction)
      { return applyDeleteListInTransaction(library, transaction, listId, options); });
  }

  async::Task<Result<DeleteListSubtreeReply>> LibraryCommands::Impl::deleteListAndDescendants(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    DeleteListOptions const options)
  {
    return detail::executeChangedWorkAsync<DeleteListSubtreeReply>(
      std::move(submission),
      "Delete list subtree",
      [this, listId, options](library::LibraryWrite& transaction)
      { return applyDeleteListSubtreeInTransaction(library, transaction, listId, options); });
  }

  async::Task<Result<DeleteListSubtreeReply>> LibraryCommands::Impl::previewDeleteListAndDescendants(
    LibraryWriteLane::Submission submission,
    ListId const listId,
    DeleteListOptions const options)
  {
    return detail::previewChangedWorkAsync<DeleteListSubtreeReply>(
      std::move(submission),
      [this, listId, options](library::LibraryWrite& transaction)
      { return applyDeleteListSubtreeInTransaction(library, transaction, listId, options); });
  }
} // namespace ao::rt
