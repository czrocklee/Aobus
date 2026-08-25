// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ao::rt
{
  class Library;
  struct MetadataPatch;
}

namespace ao::uimodel
{
  using TrackMetadataSubmitResult = rt::AuthoringResult<rt::UpdateTrackMetadataReply>;
  using TrackTagSubmitResult = rt::AuthoringResult<rt::EditTrackTagsReply>;

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

  private:
    struct Impl;
    explicit TrackAuthoringSession(std::shared_ptr<Impl> implPtr);

    std::shared_ptr<Impl> _implPtr;
  };
} // namespace ao::uimodel
