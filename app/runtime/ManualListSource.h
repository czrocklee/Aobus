// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "TrackSource.h"

#include <vector>

namespace ao::library
{
  class ListView;
}

namespace ao::rt
{
  /**
   * ManualListSource - A TrackSource that holds a manually curated set of tracks.
   *
   * It also tracks a source TrackSource to ensure its members still exist
   * and are visible in that source.
   */
  class ManualListSource final
    : public TrackSource
    , public TrackSourceObserver
  {
  public:
    explicit ManualListSource(library::ListView const& view, TrackSource* source = nullptr);
    explicit ManualListSource();
    ~ManualListSource() override;

    ManualListSource(ManualListSource const&) = delete;
    ManualListSource& operator=(ManualListSource const&) = delete;
    ManualListSource(ManualListSource&&) = delete;
    ManualListSource& operator=(ManualListSource&&) = delete;

    void reloadFromListView(library::ListView const& view);

    // TrackSource interface
    std::size_t size() const override { return _trackIds.size(); }
    TrackId trackIdAt(std::size_t index) const override { return _trackIds.at(index); }
    std::optional<std::size_t> indexOf(TrackId id) const override;

    // TrackSourceObserver interface
    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

    void onInserted(std::span<TrackId const> ids) override;
    void onUpdated(std::span<TrackId const> ids) override;
    void onRemoved(std::span<TrackId const> ids) override;

    bool contains(TrackId id) const;

    TrackSource* source() const noexcept { return _source; }
    std::vector<TrackId>& trackIds() noexcept { return _trackIds; }
    std::vector<TrackId> const& trackIds() const noexcept { return _trackIds; }

  private:
    std::vector<TrackId> _trackIds;
    TrackSource* _source = nullptr;
  };
}
