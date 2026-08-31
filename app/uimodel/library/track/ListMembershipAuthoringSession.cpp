// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/WritableTagList.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    std::string tagExpression(std::string_view const tag)
    {
      return query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = std::string{tag}});
    }

    std::size_t forgottenPositionCount(rt::AddTracksToListReply const& /*reply*/) noexcept
    {
      return 0;
    }

    std::size_t forgottenPositionCount(rt::RemoveTracksFromListReply const& reply) noexcept
    {
      return reply.forgottenPositionTrackIds.size();
    }

    std::string formatListMembershipMessage(i18n::MessageCatalog const& catalog,
                                            rt::AuthoringStatus const status,
                                            ListMembershipOperation const operation,
                                            std::string_view const listName,
                                            std::string_view const tagExpression,
                                            std::size_t const changedTrackCount,
                                            std::size_t const forgottenPositionCount)
    {
      using i18n::MessageId;
      using i18n::requiredFormat;
      using i18n::requiredText;

      if (operation == ListMembershipOperation::Add)
      {
        switch (status)
        {
          case rt::AuthoringStatus::Busy: return std::string{requiredText(catalog, MessageId::LibraryBusyTryAgain)};
          case rt::AuthoringStatus::Stale: return std::string{requiredText(catalog, MessageId::ListMembershipAddStale)};
          case rt::AuthoringStatus::Unavailable:
            return std::string{requiredText(catalog, MessageId::ListMembershipAddUnavailable)};
          case rt::AuthoringStatus::NoOp:
            return requiredFormat(
              catalog, MessageId::ListMembershipAddNoOp, {{"list", listName}, {"tag", tagExpression}});
          case rt::AuthoringStatus::Applied:
            return requiredFormat(catalog,
                                  MessageId::ListMembershipAdded,
                                  {{"tag", tagExpression}, {"count", changedTrackCount}, {"list", listName}});
        }
      }

      switch (status)
      {
        case rt::AuthoringStatus::Busy: return std::string{requiredText(catalog, MessageId::LibraryBusyTryAgain)};
        case rt::AuthoringStatus::Stale:
          return std::string{requiredText(catalog, MessageId::ListMembershipRemoveStale)};
        case rt::AuthoringStatus::Unavailable:
          return std::string{requiredText(catalog, MessageId::ListMembershipRemoveUnavailable)};
        case rt::AuthoringStatus::NoOp:
          return requiredFormat(
            catalog, MessageId::ListMembershipRemoveNoOp, {{"tag", tagExpression}, {"list", listName}});
        case rt::AuthoringStatus::Applied:
        {
          if (forgottenPositionCount == 0)
          {
            return requiredFormat(catalog,
                                  MessageId::ListMembershipRemovedWithoutPosition,
                                  {{"tag", tagExpression}, {"count", changedTrackCount}, {"list", listName}});
          }

          return requiredFormat(catalog,
                                MessageId::ListMembershipRemovedWithPositions,
                                {{"tag", tagExpression},
                                 {"trackCount", changedTrackCount},
                                 {"positionCount", forgottenPositionCount},
                                 {"list", listName}});
        }
      }

      return {};
    }
  } // namespace

  std::vector<WritableTagListTarget> writableTagListTargets(std::span<rt::ListNode const> const lists,
                                                            rt::TextOrderingPolicy const* const textOrderingPolicy)
  {
    auto result = std::vector<WritableTagListTarget>{};

    for (auto const& list : lists)
    {
      if (auto optTag = rt::writableTagForListExpression(list.expression); optTag)
      {
        result.push_back(WritableTagListTarget{
          .listId = list.id,
          .name = list.name,
          .tag = std::move(*optTag),
        });
      }
    }

    if (textOrderingPolicy == nullptr)
    {
      std::ranges::sort(result,
                        [](WritableTagListTarget const& lhs, WritableTagListTarget const& rhs)
                        {
                          if (lhs.name != rhs.name)
                          {
                            return lhs.name < rhs.name;
                          }

                          return lhs.listId < rhs.listId;
                        });
      return result;
    }

    struct OrderedTarget final
    {
      WritableTagListTarget target;
      std::string sortKey;
    };

    auto ordered = std::vector<OrderedTarget>{};
    ordered.reserve(result.size());

    for (auto& target : result)
    {
      auto sortKey = std::string{};
      auto const keyRes = textOrderingPolicy->makeSortKeyInto(sortKey, target.name);
      AO_INVARIANT(
        keyRes.has_value(), "Admitted List name failed locale sort-key derivation: {}", keyRes.error().message);
      ordered.push_back(OrderedTarget{.target = std::move(target), .sortKey = std::move(sortKey)});
    }

    std::ranges::sort(ordered,
                      [](OrderedTarget const& lhs, OrderedTarget const& rhs)
                      {
                        if (auto const sortKeyOrder = lhs.sortKey.compare(rhs.sortKey); sortKeyOrder != 0)
                        {
                          return sortKeyOrder < 0;
                        }

                        return lhs.target.listId < rhs.target.listId;
                      });

    result.clear();

    for (auto& target : ordered)
    {
      result.push_back(std::move(target.target));
    }

    return result;
  }

  struct ListMembershipAuthoringSession::State final
  {
    template<typename Submit>
    static async::Task<Result<ListMembershipEditResult>> finishEditAsync(std::shared_ptr<State> statePtr,
                                                                         ListId const listId,
                                                                         ListMembershipOperation const operation,
                                                                         Submit submit)
    {
      if (statePtr->submitting)
      {
        co_return ListMembershipEditResult{.status = rt::AuthoringStatus::Busy, .listId = listId};
      }

      statePtr->submitting = true;
      auto deferredException = std::exception_ptr{};

      try
      {
        auto result = co_await submit(*statePtr, listId);

        if (!result)
        {
          statePtr->submitting = false;
          co_return std::unexpected{result.error()};
        }

        auto completed = std::move(*result);
        auto uiResult = ListMembershipEditResult{
          .status = completed.status,
          .listId = completed.reply.listId,
          .operation = operation,
          .listName = completed.reply.listName,
          .tag = completed.reply.tag,
          .targetTrackCount = completed.reply.targetTrackIds.size(),
          .changedTrackCount = completed.reply.tagEdit.changes.size(),
          .forgottenPositionCount = forgottenPositionCount(completed.reply),
        };

        if (completed.optNextTargets)
        {
          statePtr->targets = std::move(*completed.optNextTargets);
        }

        statePtr->submitting = false;
        co_return uiResult;
      }
      catch (...)
      {
        deferredException = std::current_exception();
        statePtr->submitting = false;
        async::rethrowException(deferredException);
      }
    }

    static async::Task<Result<ListMembershipEditResult>> editAsync(std::shared_ptr<State> statePtr,
                                                                   ListId const listId,
                                                                   ListMembershipOperation const operation)
    {
      if (operation == ListMembershipOperation::Add)
      {
        return finishEditAsync(std::move(statePtr),
                               listId,
                               operation,
                               [](State& state, ListId const targetListId)
                               { return state.library.commands().addTracksToList(targetListId, state.targets); });
      }

      return finishEditAsync(std::move(statePtr),
                             listId,
                             operation,
                             [](State& state, ListId const targetListId)
                             { return state.library.commands().removeTracksFromList(targetListId, state.targets); });
    }

    rt::Library& library;
    rt::BoundTrackTargets targets;
    bool submitting = false;
  };

  std::string formatListMembershipEditNotification(i18n::MessageCatalog const& textCatalog,
                                                   ListMembershipEditResult const& result)
  {
    return formatListMembershipMessage(textCatalog,
                                       result.status,
                                       result.operation,
                                       result.listName,
                                       tagExpression(result.tag),
                                       result.changedTrackCount,
                                       result.forgottenPositionCount);
  }

  Result<ListMembershipAuthoringSession> ListMembershipAuthoringSession::begin(rt::Library& library,
                                                                               std::span<TrackId const> const trackIds)
  {
    auto targetsRes = library.bindTrackTargets(trackIds);

    if (!targetsRes)
    {
      return std::unexpected{targetsRes.error()};
    }

    return ListMembershipAuthoringSession{
      std::make_shared<State>(State{.library = library, .targets = std::move(*targetsRes)})};
  }

  ListMembershipAuthoringSession::ListMembershipAuthoringSession(std::shared_ptr<State> statePtr)
    : _statePtr{std::move(statePtr)}
  {
  }

  ListMembershipAuthoringSession::~ListMembershipAuthoringSession() = default;

  ListMembershipAuthoringSession::ListMembershipAuthoringSession(ListMembershipAuthoringSession&&) noexcept = default;

  std::span<TrackId const> ListMembershipAuthoringSession::targetIds() const noexcept
  {
    return _statePtr->targets.trackIds();
  }

  async::Task<Result<ListMembershipEditResult>> ListMembershipAuthoringSession::addToList(ListId const listId)
  {
    return State::editAsync(_statePtr, listId, ListMembershipOperation::Add);
  }

  async::Task<Result<ListMembershipEditResult>> ListMembershipAuthoringSession::removeFromList(ListId const listId)
  {
    return State::editAsync(_statePtr, listId, ListMembershipOperation::Remove);
  }
} // namespace ao::uimodel
