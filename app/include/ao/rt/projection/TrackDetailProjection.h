// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "../ViewIds.h"
#include "TrackDetailSnapshot.h"
#include <ao/async/Subscription.h>

#include <functional>
#include <variant>
#include <vector>

namespace ao::rt
{
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

  class TrackDetailProjection
  {
  public:
    virtual ~TrackDetailProjection() = default;

    TrackDetailProjection(TrackDetailProjection const&) = delete;
    TrackDetailProjection& operator=(TrackDetailProjection const&) = delete;
    TrackDetailProjection(TrackDetailProjection&&) = delete;
    TrackDetailProjection& operator=(TrackDetailProjection&&) = delete;

    virtual TrackDetailSnapshot snapshot() const = 0;
    virtual async::Subscription subscribe(
      std::move_only_function<void(TrackDetailSnapshot const&) noexcept> handler) = 0;

  protected:
    TrackDetailProjection() = default;
  };
} // namespace ao::rt
