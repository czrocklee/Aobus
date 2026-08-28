// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <cstddef>
#include <cstdint>
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
  /** Result and session types for revision-bound track authoring. */
  using TrackMetadataSubmitResult = rt::AuthoringResult<rt::UpdateTrackMetadataReply>;
  using TrackTagSubmitResult = rt::AuthoringResult<rt::EditTrackTagsReply>;
  using TrackPropertiesSubmitResult = rt::AuthoringResult<rt::UpdateTrackPropertiesReply>;

  /**
   * Platform-neutral lifetime for one stable set of authoring targets.
   *
   * The session owns the runtime binding and becomes stale on maintenance,
   * runtime replacement, or any intervening effective library commit. A stale
   * session never silently rebinds its draft to newer projection state.
   */
  class [[nodiscard]] TrackAuthoringSession final
  {
  public:
    static Result<std::unique_ptr<TrackAuthoringSession>> begin(rt::Library& library,
                                                                std::span<TrackId const> targetIds);

    ~TrackAuthoringSession();

    TrackAuthoringSession(TrackAuthoringSession const&) = delete;
    TrackAuthoringSession& operator=(TrackAuthoringSession const&) = delete;
    TrackAuthoringSession(TrackAuthoringSession&&) = delete;
    TrackAuthoringSession& operator=(TrackAuthoringSession&&) = delete;

    bool isCurrent() const noexcept;
    std::span<TrackId const> targetIds() const noexcept;
    async::Subscription onInvalidated(compat::MoveOnlyFunction<void()> handler) const;

    async::Task<Result<TrackMetadataSubmitResult>> submitMetadata(rt::MetadataPatch patch);
    async::Task<Result<TrackTagSubmitResult>> submitTags(std::vector<std::string> tagsToAdd,
                                                         std::vector<std::string> tagsToRemove);
    async::Task<Result<TrackPropertiesSubmitResult>> submitProperties(rt::TrackPropertiesPatch patch);

  private:
    struct Impl;
    explicit TrackAuthoringSession(std::shared_ptr<Impl> implPtr);

    std::shared_ptr<Impl> _implPtr;
  };

  enum class ListMembershipOperation : std::uint8_t
  {
    Add,
    Remove,
  };

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
    ListMembershipOperation operation = ListMembershipOperation::Add;
    std::string listName{};
    std::string tag{};
    std::size_t targetTrackCount = 0;
    std::size_t changedTrackCount = 0;
    std::size_t forgottenPositionCount = 0;

    bool operator==(ListMembershipEditResult const&) const = default;
  };

  std::string formatListMembershipEditNotification(i18n::MessageCatalog const& textCatalog,
                                                   ListMembershipEditResult const& result);

  class [[nodiscard]] ListMembershipAuthoringSession final
  {
  public:
    static Result<std::unique_ptr<ListMembershipAuthoringSession>> begin(rt::Library& library,
                                                                         std::span<TrackId const> trackIds);

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
