// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ComponentRegistry.h"
#include <ao/rt/ProjectionTypes.h>

#include <sigc++/signal.h>

#include <cstdint>

namespace ao::gtk::layout
{
  class ITrackDetailScope
  {
  public:
    ITrackDetailScope() = default;
    virtual ~ITrackDetailScope() = default;
    ITrackDetailScope(ITrackDetailScope const&) = delete;
    ITrackDetailScope& operator=(ITrackDetailScope const&) = delete;
    ITrackDetailScope(ITrackDetailScope&&) = delete;
    ITrackDetailScope& operator=(ITrackDetailScope&&) = delete;

    virtual rt::TrackDetailSnapshot const& snapshot() const = 0;
    virtual bool isEditLocked() const = 0;
    virtual void setEditLocked(bool locked) = 0;

    virtual sigc::signal<void(rt::TrackDetailSnapshot const&)>& signalSnapshotChanged() = 0;
    virtual sigc::signal<void(bool)>& signalEditLockChanged() = 0;
  };

  enum class LayoutMode : std::uint8_t
  {
    Standard,
    Wide
  };

  namespace detail
  {
    LayoutMode computeLayoutMode(std::int32_t width);
    std::int32_t coverArtSideForWidth(std::int32_t width, std::int32_t targetSize);
  }

  /**
   * @brief Register track detail layout components.
   */
  void registerTrackDetailComponents(ComponentRegistry& registry);
} // namespace ao::gtk::layout
