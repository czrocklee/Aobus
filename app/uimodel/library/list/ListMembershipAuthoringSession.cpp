// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/list/ListMembershipAuthoringSession.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/WritableTagList.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>

#include <algorithm>
#include <expected>
#include <format>
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

    std::string addNotification(ListMembershipEditResult const& result)
    {
      switch (result.status)
      {
        case rt::TrackAuthoringStatus::Stale: return "The library changed before tracks could be added. Try again.";
        case rt::TrackAuthoringStatus::Unavailable: return "Library is busy. Tracks were not added.";
        case rt::TrackAuthoringStatus::NoOp:
          return std::format("{} already has {} on the selected tracks.", result.listName, tagExpression(result.tag));
        case rt::TrackAuthoringStatus::Applied:
          return std::format("Added {} to {} track{} in {}.",
                             tagExpression(result.tag),
                             result.changedTrackCount,
                             result.changedTrackCount == 1 ? "" : "s",
                             result.listName);
      }

      return {};
    }

    std::string removeNotification(ListMembershipEditResult const& result)
    {
      switch (result.status)
      {
        case rt::TrackAuthoringStatus::Stale: return "The library changed before tracks could be removed. Try again.";
        case rt::TrackAuthoringStatus::Unavailable: return "Library is busy. Tracks were not removed.";
        case rt::TrackAuthoringStatus::NoOp:
          return std::format(
            "No {} membership or saved position remained in {}.", tagExpression(result.tag), result.listName);
        case rt::TrackAuthoringStatus::Applied: break;
      }

      if (result.forgottenPositionCount == 0)
      {
        return std::format("Removed {} from {} track{} and confirmed no saved position remains in {}.",
                           tagExpression(result.tag),
                           result.changedTrackCount,
                           result.changedTrackCount == 1 ? "" : "s",
                           result.listName);
      }

      return std::format("Removed {} from {} track{} and forgot {} saved position{} in {}.",
                         tagExpression(result.tag),
                         result.changedTrackCount,
                         result.changedTrackCount == 1 ? "" : "s",
                         result.forgottenPositionCount,
                         result.forgottenPositionCount == 1 ? "" : "s",
                         result.listName);
    }
  } // namespace

  std::vector<WritableTagListTarget> writableTagListTargets(std::span<rt::ListNode const> const lists)
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

  struct ListMembershipAuthoringSession::Impl final
  {
    rt::Library& library;
    rt::BoundTrackTargets targets;
  };

  Result<std::unique_ptr<ListMembershipAuthoringSession>> ListMembershipAuthoringSession::begin(
    rt::Library& library,
    std::span<TrackId const> const trackIds)
  {
    auto targetsRes = library.bindTrackTargets(trackIds);

    if (!targetsRes)
    {
      return std::unexpected{targetsRes.error()};
    }

    return std::unique_ptr<ListMembershipAuthoringSession>{new ListMembershipAuthoringSession{
      std::make_unique<Impl>(Impl{.library = library, .targets = std::move(*targetsRes)})}};
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
    uiResult.notificationText = addNotification(uiResult);

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
    uiResult.notificationText = removeNotification(uiResult);

    if (result->optNextTargets)
    {
      _implPtr->targets = *result->optNextTargets;
    }

    return uiResult;
  }
} // namespace ao::uimodel
