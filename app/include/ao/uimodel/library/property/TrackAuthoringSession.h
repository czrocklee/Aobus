// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/LibraryAuthoring.h>

#include <functional>
#include <memory>
#include <span>
#include <string>

namespace ao::rt
{
  class Library;
  struct MetadataPatch;
}

namespace ao::uimodel
{
  template<typename Reply>
  struct TrackAuthoringSubmitResult final
  {
    rt::TrackAuthoringStatus status = rt::TrackAuthoringStatus::NoOp;
    Reply reply{};
  };

  using TrackMetadataSubmitResult = TrackAuthoringSubmitResult<rt::UpdateTrackMetadataReply>;
  using TrackTagSubmitResult = TrackAuthoringSubmitResult<rt::EditTrackTagsReply>;

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
    async::Subscription onInvalidated(std::move_only_function<void()> handler) const;

    Result<TrackMetadataSubmitResult> submitMetadata(rt::MetadataPatch const& patch);
    Result<TrackTagSubmitResult> submitTags(std::span<std::string const> tagsToAdd,
                                            std::span<std::string const> tagsToRemove);

  private:
    struct Impl;
    explicit TrackAuthoringSession(std::unique_ptr<Impl> implPtr);

    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::uimodel
