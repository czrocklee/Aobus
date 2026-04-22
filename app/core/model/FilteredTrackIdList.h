// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/model/TrackIdList.h"

#include <rs/core/MusicLibrary.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/PlanEvaluator.h>

#include <memory>
#include <string>

namespace app::core::model
{

  // Forward declaration - include SmartListEngine.h to get full type
  class SmartListEngine;

  /**
   * FilteredTrackIdList - Facade for smart list membership backed by SmartListEngine.
   *
   * Acts as a TrackIdList adapter that forwards requests to SmartListEngine
   * and relays notifications to observers.
   *
   * This class no longer directly evaluates expressions or reads tracks -
   * that is handled by SmartListEngine for optimal batching.
   *
   * Inherited Filtering:
   * FilteredTrackIdList implements inherited smart-list semantics by:
   * 1. Taking a source TrackIdList as its parent membership
   * 2. Applying the local expression filter to that source
   * 3. The effective membership becomes: source_tracks WHERE expression
   *
   * This enables chains like:
   *   AllTracks → FilteredTrackIdList(parent=Rock Album, expr="artist='Beatles'")
   *     → FilteredTrackIdList(parent=Beatles Tracks, expr="year > 1970")
   *
   * The chain is built by the caller (e.g., MainWindow::buildPageForStoredList)
   * which passes the parent's membership list as the source for the child.
   */
  class FilteredTrackIdList final : public TrackIdList
  {
  public:
    FilteredTrackIdList(TrackIdList& source, rs::core::MusicLibrary& ml, SmartListEngine& engine);
    ~FilteredTrackIdList() override;

    void setExpression(std::string expr);
    void reload();

    // TrackIdList interface
    std::size_t size() const override;
    TrackId trackIdAt(std::size_t index) const override;
    std::optional<std::size_t> indexOf(TrackId id) const override;

    // Override to notify engine when track data changes (e.g., tag added/removed)
    void notifyTrackDataChanged(TrackId id) override;

    bool hasError() const;
    std::string const& errorMessage() const;

  private:
    friend class SmartListEngine;

    // Methods called by SmartListEngine to notify this facade
    void notifyEngineReset();
    void notifyEngineInserted(TrackId id, std::size_t index);
    void notifyEngineUpdated(TrackId id, std::size_t index);
    void notifyEngineRemoved(TrackId id, std::size_t index);

    SmartListEngine* engine() const;

    SmartListEngine* _engine = nullptr;
    std::uint64_t _registrationId = 0; // Use std::uint64_t directly instead of RegistrationId
  };

} // namespace app::core::model