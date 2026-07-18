// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackMutation.h>

#include <cstdint>
#include <functional>
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
  enum class TrackAuthoringSessionState : std::uint8_t
  {
    Editing,
    Submitting,
    Applied,
    Stale,
    Rejected,
  };

  enum class TrackAuthoringSubmitStatus : std::uint8_t
  {
    Applied,
    NoOp,
    Stale,
    Missing,
    Unavailable,
  };

  template<typename Reply>
  struct TrackAuthoringSubmitResult final
  {
    TrackAuthoringSubmitStatus status = TrackAuthoringSubmitStatus::NoOp;
    Reply reply{};
    std::vector<TrackId> missingTargetIds{};
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

    TrackAuthoringSessionState state() const noexcept;
    std::span<TrackId const> targetIds() const noexcept;
    async::Subscription onStateChanged(std::move_only_function<void(TrackAuthoringSessionState)> handler) const;

    Result<TrackMetadataSubmitResult> submitMetadata(rt::MetadataPatch const& patch);
    Result<TrackTagSubmitResult> submitTags(std::span<std::string const> tagsToAdd,
                                            std::span<std::string const> tagsToRemove);

  private:
    struct Impl;
    explicit TrackAuthoringSession(std::unique_ptr<Impl> implPtr);

    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::uimodel
