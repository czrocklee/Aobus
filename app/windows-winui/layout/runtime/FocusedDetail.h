// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <memory>

namespace ao::rt
{
  class TrackDetailProjection;
  class WorkspaceService;
}

namespace ao::winui::layout
{
  /**
   * @brief The one focused-selection projection a generation's components share.
   *
   * Building a snapshot reads every selected track out of the library and a
   * shell shows one focused selection, so the components that follow it read
   * the same projection rather than each aggregating the selection again. It is
   * made on the first ask: a document that shows no track detail pays nothing.
   *
   * A projection belongs to the runtime that made it, so replacing the library
   * invalidates it. Components hold the holder rather than the projection,
   * which is what lets them all move to the next one without any of them owning
   * the decision or having to agree on who makes it.
   */
  class FocusedDetail final
  {
  public:
    /// The projection over this window's runtime, made on the first ask.
    std::shared_ptr<rt::TrackDetailProjection> projection(rt::WorkspaceService& workspace);

  private:
    std::shared_ptr<rt::TrackDetailProjection> _projectionPtr;
  };
} // namespace ao::winui::layout
