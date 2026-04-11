// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "TrackIdList.h"

#include <rs/core/MusicLibrary.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/PlanEvaluator.h>

#include <memory>
#include <string>

namespace app::model
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
   */
  class FilteredTrackIdList final : public TrackIdList
  {
  public:
    FilteredTrackIdList(TrackIdList& source,
                        rs::core::MusicLibrary& ml,
                        SmartListEngine& engine);
    ~FilteredTrackIdList() override;

    void setExpression(std::string expr);
    void reload();

    // TrackIdList interface
    [[nodiscard]] std::size_t size() const override;
    [[nodiscard]] TrackId trackIdAt(std::size_t index) const override;
    [[nodiscard]] std::optional<std::size_t> indexOf(TrackId id) const override;

    bool hasError() const;
    std::string const& errorMessage() const;

  private:
    friend class SmartListEngine;

    // Methods called by SmartListEngine to notify this facade
    void notifyEngineReset();
    void notifyEngineInserted(TrackId id, std::size_t index);
    void notifyEngineUpdated(TrackId id, std::size_t index);
    void notifyEngineRemoved(TrackId id, std::size_t index);

    SmartListEngine* _engine = nullptr;
    std::uint64_t _registrationId = 0;  // Use std::uint64_t directly instead of RegistrationId
  };

} // namespace app::model