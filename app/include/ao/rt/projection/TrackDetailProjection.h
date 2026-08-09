// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../ViewIds.h"
#include "TrackDetailSnapshot.h"
#include <ao/CoreIds.h>
#include <ao/async/Subscription.h>

#include <functional>
#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class LibraryChanges;
  class ViewService;
  class WorkspaceService;

  struct FocusedViewTarget final
  {};
  struct ExplicitViewTarget final
  {
    ViewId viewId{};
  };
  struct ExplicitSelectionTarget final
  {
    std::vector<TrackId> trackIds{};
  };

  using DetailTarget = std::variant<FocusedViewTarget, ExplicitViewTarget, ExplicitSelectionTarget>;

  class TrackDetailProjection final
  {
  public:
    TrackDetailProjection(DetailTarget target,
                          ViewService& views,
                          library::MusicLibrary const& library,
                          WorkspaceService& workspace,
                          LibraryChanges const& changes);
    ~TrackDetailProjection();

    TrackDetailProjection(TrackDetailProjection const&) = delete;
    TrackDetailProjection& operator=(TrackDetailProjection const&) = delete;
    TrackDetailProjection(TrackDetailProjection&&) = delete;
    TrackDetailProjection& operator=(TrackDetailProjection&&) = delete;

    TrackDetailSnapshot snapshot() const;
    async::Subscription subscribe(std::move_only_function<void(TrackDetailSnapshot const&)> handler);

  private:
    TrackDetailSnapshot buildSnapshot(std::span<TrackId const> ids) const;
    void refreshSnapshot(std::span<TrackId const> ids);
    void publishSnapshot();

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
