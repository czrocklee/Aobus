// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListMembershipAuthoringSession.h>

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
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/uimodel/presentation/PresentationText.h>

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

  struct ListMembershipAuthoringSession::Impl final
  {
    template<typename Submit>
    static async::Task<Result<ListMembershipEditResult>> finishEditAsync(std::shared_ptr<Impl> implPtr,
                                                                         ListId const listId,
                                                                         ListMembershipOperation const operation,
                                                                         Submit submit)
    {
      if (implPtr->submitting)
      {
        co_return ListMembershipEditResult{.status = rt::AuthoringStatus::Busy, .listId = listId};
      }

      implPtr->submitting = true;
      auto deferredException = std::exception_ptr{};

      try
      {
        auto result = co_await submit(*implPtr, listId);

        if (!result)
        {
          implPtr->submitting = false;
          co_return std::unexpected{result.error()};
        }

        auto completed = std::move(*result);
        auto uiResult = ListMembershipEditResult{
          .status = completed.status,
          .listId = completed.reply.listId,
          .listName = completed.reply.listName,
          .tag = completed.reply.tag,
          .targetTrackCount = completed.reply.targetTrackIds.size(),
          .changedTrackCount = completed.reply.tagEdit.changes.size(),
          .forgottenPositionCount = forgottenPositionCount(completed.reply),
        };
        uiResult.notificationText = formatListMembershipMessage(implPtr->textCatalog,
                                                                uiResult.status,
                                                                operation,
                                                                uiResult.listName,
                                                                tagExpression(uiResult.tag),
                                                                uiResult.changedTrackCount,
                                                                uiResult.forgottenPositionCount);

        if (completed.optNextTargets)
        {
          implPtr->targets = std::move(*completed.optNextTargets);
        }

        implPtr->submitting = false;
        co_return uiResult;
      }
      catch (...)
      {
        deferredException = std::current_exception();
        implPtr->submitting = false;
        async::rethrowException(deferredException);
      }
    }

    static async::Task<Result<ListMembershipEditResult>> editAsync(std::shared_ptr<Impl> implPtr,
                                                                   ListId const listId,
                                                                   ListMembershipOperation const operation)
    {
      if (operation == ListMembershipOperation::Add)
      {
        return finishEditAsync(std::move(implPtr),
                               listId,
                               operation,
                               [](Impl& impl, ListId const targetListId)
                               { return impl.library.writer().addTracksToList(targetListId, impl.targets); });
      }

      return finishEditAsync(std::move(implPtr),
                             listId,
                             operation,
                             [](Impl& impl, ListId const targetListId)
                             { return impl.library.writer().removeTracksFromList(targetListId, impl.targets); });
    }

    rt::Library& library;
    rt::BoundTrackTargets targets;
    i18n::MessageCatalog textCatalog;
    bool submitting = false;
  };

  Result<std::unique_ptr<ListMembershipAuthoringSession>> ListMembershipAuthoringSession::begin(
    rt::Library& library,
    std::span<TrackId const> const trackIds,
    i18n::MessageCatalog const& textCatalog)
  {
    auto targetsRes = library.bindTrackTargets(trackIds);

    if (!targetsRes)
    {
      return std::unexpected{targetsRes.error()};
    }

    return std::unique_ptr<ListMembershipAuthoringSession>{new ListMembershipAuthoringSession{
      std::make_shared<Impl>(Impl{.library = library, .targets = std::move(*targetsRes), .textCatalog = textCatalog})}};
  }

  ListMembershipAuthoringSession::ListMembershipAuthoringSession(std::shared_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  ListMembershipAuthoringSession::~ListMembershipAuthoringSession() = default;

  std::span<TrackId const> ListMembershipAuthoringSession::targetIds() const noexcept
  {
    return _implPtr->targets.trackIds();
  }

  async::Task<Result<ListMembershipEditResult>> ListMembershipAuthoringSession::addToList(ListId const listId)
  {
    return Impl::editAsync(_implPtr, listId, ListMembershipOperation::Add);
  }

  async::Task<Result<ListMembershipEditResult>> ListMembershipAuthoringSession::removeFromList(ListId const listId)
  {
    return Impl::editAsync(_implPtr, listId, ListMembershipOperation::Remove);
  }
} // namespace ao::uimodel
