// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/model/TrackIdList.h"

#include <vector>

namespace rs::core
{
  class ListView;
}

namespace app::core::model
{

  /**
   * ManualTrackIdList - A TrackIdList that holds a manually curated set of tracks.
   * Backed by a ListRecord from the library.
   *
   * It also tracks a source TrackIdList to ensure its members still exist
   * and are visible in that source.
   */
  class ManualTrackIdList final
    : public TrackIdList
    , public TrackIdListObserver
  {
  public:
    explicit ManualTrackIdList(rs::core::ListView const& view, TrackIdList* source = nullptr);
    explicit ManualTrackIdList();
    ~ManualTrackIdList() override;

    void reloadFromListView(rs::core::ListView const& view);

    // TrackIdList interface
    std::size_t size() const override { return _trackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _trackIds.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    // TrackIdListObserver interface
    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

    void onBatchInserted(std::span<const TrackId> ids) override;
    void onBatchUpdated(std::span<const TrackId> ids) override;
    void onBatchRemoved(std::span<const TrackId> ids) override;

    bool contains(TrackId id) const;

    std::vector<TrackId> _trackIds;
    TrackIdList* _source = nullptr;
  };

} // namespace app::core::model
