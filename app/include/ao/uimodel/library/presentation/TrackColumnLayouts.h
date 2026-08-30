// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackField.h>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace ao::rt
{
  class LibraryChanges;
}

namespace ao::uimodel
{
  struct TrackColumnState final
  {
    rt::TrackField field = rt::TrackField::Title;
    std::int32_t width = -1;
    double weight = -1.0;
    bool visible = true;

    bool operator==(TrackColumnState const&) const = default;
  };

  class TrackColumnLayouts final
  {
  public:
    using Snapshot = std::map<ListId, std::vector<TrackColumnState>>;

    TrackColumnLayouts() = default;
    explicit TrackColumnLayouts(rt::LibraryChanges const& changes);
    ~TrackColumnLayouts();

    TrackColumnLayouts(TrackColumnLayouts const&) = delete;
    TrackColumnLayouts& operator=(TrackColumnLayouts const&) = delete;
    TrackColumnLayouts(TrackColumnLayouts&&) = delete;
    TrackColumnLayouts& operator=(TrackColumnLayouts&&) = delete;

    Snapshot snapshot() const { return _listLayouts; }
    void restore(Snapshot layouts);

    std::vector<TrackColumnState> const& layoutForList(ListId listId) const noexcept;
    void updateLayout(ListId listId, std::vector<TrackColumnState> const& layout);

    async::Signal<ListId>& signalChanged() noexcept { return *_changedPtr; }

  private:
    std::map<ListId, std::vector<TrackColumnState>> _listLayouts{};
    std::shared_ptr<async::Signal<ListId>> _changedPtr{std::make_shared<async::Signal<ListId>>()};
    async::Subscription _changesSubscription;
  };

  std::vector<rt::TrackField> visibleTrackFieldsInStoredOrder(std::span<rt::TrackField const> visibleFields,
                                                              std::span<rt::TrackField const> storedOrder);
  std::vector<rt::TrackField> visibleTrackFieldsInStoredLayout(std::span<rt::TrackField const> presentationFields,
                                                               std::span<TrackColumnState const> storedLayout);
  std::vector<TrackColumnState> mergeVisibleTrackColumnLayout(std::span<TrackColumnState const> storedLayout,
                                                              std::span<TrackColumnState const> visibleLayout);
} // namespace ao::uimodel
