// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/projection/TrackDetailProjection.h>

#include <sigc++/signal.h>

namespace ao::gtk::layout
{
  /**
   * @brief Interface for track detail data scope providers.
   */
  class TrackDetailScope
  {
  public:
    TrackDetailScope() = default;
    virtual ~TrackDetailScope() = default;
    TrackDetailScope(TrackDetailScope const&) = delete;
    TrackDetailScope& operator=(TrackDetailScope const&) = delete;
    TrackDetailScope(TrackDetailScope&&) = delete;
    TrackDetailScope& operator=(TrackDetailScope&&) = delete;

    virtual rt::TrackDetailSnapshot const& snapshot() const = 0;

    virtual sigc::signal<void(rt::TrackDetailSnapshot const&)>& signalSnapshotChanged() = 0;
  };
} // namespace ao::gtk::layout
