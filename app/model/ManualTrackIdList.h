// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "TrackIdList.h"

#include <rs/core/ListView.h>

#include <vector>

namespace app::model
{

  /**
   * ManualTrackIdList - Manual list membership loaded from ListView::trackIds().
   * Preserves the stored TrackId order (does not sort).
   * Optionally observes a source list to react to track updates/removes.
   */
  class ManualTrackIdList final
    : public TrackIdList
    , public TrackIdListObserver
  {
  public:
    explicit ManualTrackIdList(rs::core::ListView const& view, TrackIdList* source = nullptr);
    explicit ManualTrackIdList();

    ~ManualTrackIdList() override;

    void reload(rs::core::ListView const& view);

  private:
    std::size_t size() const override { return _trackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _trackIds.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    // TrackIdListObserver interface
    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

    bool contains(TrackId id) const;

    std::vector<TrackId> _trackIds;
    TrackIdList* _source = nullptr;
  };

} // namespace app::model