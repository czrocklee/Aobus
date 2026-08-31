// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/uimodel/library/list/ListOrder.h>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ao::rt
{
  class Library;
  class ViewService;
}

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
{
  /**
   * Revision- and view-bound editing for one effective List order.
   *
   * Submitted operations retain the session State, but Library and ViewService
   * remain borrows and must outlive every such operation.
   */
  class [[nodiscard]] ListOrderAuthoringSession final
  {
  public:
    static Result<ListOrderAuthoringSession> begin(rt::Library& library,
                                                   rt::ViewService& views,
                                                   rt::ViewId viewId,
                                                   i18n::MessageCatalog const& textCatalog);

    ~ListOrderAuthoringSession();

    ListOrderAuthoringSession(ListOrderAuthoringSession const&) = delete;
    ListOrderAuthoringSession& operator=(ListOrderAuthoringSession const&) = delete;
    ListOrderAuthoringSession(ListOrderAuthoringSession&&) noexcept;
    ListOrderAuthoringSession& operator=(ListOrderAuthoringSession&&) = delete;

    bool isCurrent() const noexcept;
    ListOrderCapabilityState const& capabilities() const noexcept;
    std::span<TrackId const> effectiveTrackIds() const noexcept;
    async::Subscription onInvalidated(compat::MoveOnlyFunction<void()> handler) const;

    async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> moveBefore(
      std::vector<TrackId> selectedTrackIds,
      std::optional<TrackId> optBeforeTrackId);
    async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> moveUp(std::vector<TrackId> selectedTrackIds);
    async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> moveDown(std::vector<TrackId> selectedTrackIds);
    async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> moveToTop(std::vector<TrackId> selectedTrackIds);
    async::Task<Result<rt::AuthoringResult<rt::MoveListOrderReply>>> moveToBottom(
      std::vector<TrackId> selectedTrackIds);
    async::Task<Result<rt::AuthoringResult<rt::ResetListOrderReply>>> resetOrder();
    async::Task<Result<rt::AuthoringResult<rt::ForgetHiddenListOrderReply>>> forgetHiddenPositions();

  private:
    struct State;
    explicit ListOrderAuthoringSession(std::shared_ptr<State> statePtr);

    std::shared_ptr<State> _statePtr;
  };
} // namespace ao::uimodel
