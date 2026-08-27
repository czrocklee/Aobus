// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ao::rt
{
  class Library;
  class TextOrderingPolicy;
}

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
{
  struct WritableTagListTarget final
  {
    ListId listId = kInvalidListId;
    std::string name{};
    std::string tag{};

    bool operator==(WritableTagListTarget const&) const = default;
  };

  std::vector<WritableTagListTarget> writableTagListTargets(std::span<rt::ListNode const> lists,
                                                            rt::TextOrderingPolicy const* textOrderingPolicy = nullptr);

  struct ListMembershipEditResult final
  {
    rt::AuthoringStatus status = rt::AuthoringStatus::NoOp;
    ListId listId = kInvalidListId;
    std::string listName{};
    std::string tag{};
    std::size_t targetTrackCount = 0;
    std::size_t changedTrackCount = 0;
    std::size_t forgottenPositionCount = 0;
    std::string notificationText{};

    bool operator==(ListMembershipEditResult const&) const = default;
  };

  class [[nodiscard]] ListMembershipAuthoringSession final
  {
  public:
    static Result<std::unique_ptr<ListMembershipAuthoringSession>> begin(rt::Library& library,
                                                                         std::span<TrackId const> trackIds,
                                                                         i18n::MessageCatalog const& textCatalog);

    ~ListMembershipAuthoringSession();

    ListMembershipAuthoringSession(ListMembershipAuthoringSession const&) = delete;
    ListMembershipAuthoringSession& operator=(ListMembershipAuthoringSession const&) = delete;
    ListMembershipAuthoringSession(ListMembershipAuthoringSession&&) = delete;
    ListMembershipAuthoringSession& operator=(ListMembershipAuthoringSession&&) = delete;

    std::span<TrackId const> targetIds() const noexcept;
    async::Task<Result<ListMembershipEditResult>> addToList(ListId listId);
    async::Task<Result<ListMembershipEditResult>> removeFromList(ListId listId);

  private:
    struct Impl;
    explicit ListMembershipAuthoringSession(std::shared_ptr<Impl> implPtr);

    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::uimodel
