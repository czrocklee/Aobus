// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/model/TrackIdList.h>

#include <vector>

namespace rs::library
{
  class ListView;
}

namespace rs::model
{

  /**
   * ManualTrackIdList - A TrackIdList that holds a manually curated set of tracks.
   *
   * It also tracks a source TrackIdList to ensure its members still exist
   * and are visible in that source.
   */
  class ManualTrackIdList final
    : public TrackIdList
    , public TrackIdListObserver
  {
  public:
    explicit ManualTrackIdList(rs::library::ListView const& view, TrackIdList* source = nullptr);
    explicit ManualTrackIdList();
    ~ManualTrackIdList() override;

    void reloadFromListView(rs::library::ListView const& view);

    // TrackIdList interface
    std::size_t size() const override { return _trackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _trackIds.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    // TrackIdListObserver interface
    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

    void onBatchInserted(std::span<TrackId const> ids) override;
    void onBatchUpdated(std::span<TrackId const> ids) override;
    void onBatchRemoved(std::span<TrackId const> ids) override;

    bool contains(TrackId id) const;

    std::vector<TrackId> _trackIds;
    TrackIdList* _source = nullptr;
  };

} // namespace rs::model
