// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/list/ListOrderPolicy.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace ao::rt
{
  class Library;
  class ViewService;
}

namespace ao::uimodel
{
  class [[nodiscard]] ListOrderAuthoringSession final
  {
  public:
    static Result<std::unique_ptr<ListOrderAuthoringSession>> begin(rt::Library& library,
                                                                    rt::ViewService& views,
                                                                    rt::ViewId viewId);

    ~ListOrderAuthoringSession();

    ListOrderAuthoringSession(ListOrderAuthoringSession const&) = delete;
    ListOrderAuthoringSession& operator=(ListOrderAuthoringSession const&) = delete;
    ListOrderAuthoringSession(ListOrderAuthoringSession&&) = delete;
    ListOrderAuthoringSession& operator=(ListOrderAuthoringSession&&) = delete;

    bool isCurrent() const noexcept;
    ListOrderCapabilityState const& capabilities() const noexcept;
    std::span<TrackId const> effectiveTrackIds() const noexcept;
    async::Subscription onInvalidated(std::move_only_function<void()> handler) const;

    Result<rt::LibraryWriter::MoveOrderAuthoringResult> moveBefore(std::span<TrackId const> selectedTrackIds,
                                                                   std::optional<TrackId> optBeforeTrackId);
    Result<rt::LibraryWriter::MoveOrderAuthoringResult> moveUp(std::span<TrackId const> selectedTrackIds);
    Result<rt::LibraryWriter::MoveOrderAuthoringResult> moveDown(std::span<TrackId const> selectedTrackIds);
    Result<rt::LibraryWriter::MoveOrderAuthoringResult> moveToTop(std::span<TrackId const> selectedTrackIds);
    Result<rt::LibraryWriter::MoveOrderAuthoringResult> moveToBottom(std::span<TrackId const> selectedTrackIds);
    Result<rt::LibraryWriter::ResetOrderAuthoringResult> resetOrder();
    Result<rt::LibraryWriter::ForgetHiddenOrderAuthoringResult> forgetHiddenPositions();

  private:
    struct Impl;
    explicit ListOrderAuthoringSession(std::unique_ptr<Impl> implPtr);

    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::uimodel
