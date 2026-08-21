// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListMembershipAuthoringSession.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/WritableTagList.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <algorithm>
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
    rt::Library& library;
    rt::BoundTrackTargets targets;
    PresentationTextCatalog textCatalog;
  };

  Result<std::unique_ptr<ListMembershipAuthoringSession>> ListMembershipAuthoringSession::begin(
    rt::Library& library,
    std::span<TrackId const> const trackIds,
    PresentationTextCatalog const& textCatalog)
  {
    auto targetsRes = library.bindTrackTargets(trackIds);

    if (!targetsRes)
    {
      return std::unexpected{targetsRes.error()};
    }

    return std::unique_ptr<ListMembershipAuthoringSession>{new ListMembershipAuthoringSession{
      std::make_unique<Impl>(Impl{.library = library, .targets = std::move(*targetsRes), .textCatalog = textCatalog})}};
  }

  ListMembershipAuthoringSession::ListMembershipAuthoringSession(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  ListMembershipAuthoringSession::~ListMembershipAuthoringSession() = default;

  std::span<TrackId const> ListMembershipAuthoringSession::targetIds() const noexcept
  {
    return _implPtr->targets.trackIds();
  }

  Result<ListMembershipEditResult> ListMembershipAuthoringSession::addToList(ListId const listId)
  {
    auto result = _implPtr->library.writer().addTracksToList(listId, _implPtr->targets);

    if (!result)
    {
      return std::unexpected{result.error()};
    }

    auto uiResult = ListMembershipEditResult{
      .status = result->status,
      .listId = result->reply.listId,
      .listName = result->reply.listName,
      .tag = result->reply.tag,
      .targetTrackCount = result->reply.targetTrackIds.size(),
      .changedTrackCount = result->reply.tagEdit.changes.size(),
    };
    uiResult.notificationText = _implPtr->textCatalog.listMembershipNotification(uiResult.status,
                                                                                 ListMembershipOperation::Add,
                                                                                 uiResult.listName,
                                                                                 tagExpression(uiResult.tag),
                                                                                 uiResult.changedTrackCount,
                                                                                 uiResult.forgottenPositionCount);

    if (result->optNextTargets)
    {
      _implPtr->targets = *result->optNextTargets;
    }

    return uiResult;
  }

  Result<ListMembershipEditResult> ListMembershipAuthoringSession::removeFromList(ListId const listId)
  {
    auto result = _implPtr->library.writer().removeTracksFromList(listId, _implPtr->targets);

    if (!result)
    {
      return std::unexpected{result.error()};
    }

    auto uiResult = ListMembershipEditResult{
      .status = result->status,
      .listId = result->reply.listId,
      .listName = result->reply.listName,
      .tag = result->reply.tag,
      .targetTrackCount = result->reply.targetTrackIds.size(),
      .changedTrackCount = result->reply.tagEdit.changes.size(),
      .forgottenPositionCount = result->reply.forgottenPositionTrackIds.size(),
    };
    uiResult.notificationText = _implPtr->textCatalog.listMembershipNotification(uiResult.status,
                                                                                 ListMembershipOperation::Remove,
                                                                                 uiResult.listName,
                                                                                 tagExpression(uiResult.tag),
                                                                                 uiResult.changedTrackCount,
                                                                                 uiResult.forgottenPositionCount);

    if (result->optNextTargets)
    {
      _implPtr->targets = *result->optNextTargets;
    }

    return uiResult;
  }
} // namespace ao::uimodel
