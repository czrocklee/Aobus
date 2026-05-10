// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <runtime/StateTypes.h>

#include <sigc++/sigc++.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  enum class TrackColumn : std::uint8_t
  {
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Composer,
    Work,
    Year,
    DiscNumber,
    TrackNumber,
    Title,
    Duration,
    Tags,
  };

  struct TrackColumnDefinition
  {
    TrackColumn column;
    std::string_view id;
    std::string_view title;
    std::int32_t defaultWidth = -1;
    bool defaultVisible = true;
    bool expands = false;
    bool numeric = false;
    bool tagsCell = false;
  };

  struct TrackColumnState
  {
    TrackColumn column = TrackColumn::Title;
    bool visible = true;
    std::int32_t width = -1;

    auto operator==(TrackColumnState const&) const -> bool = default;
  };

  struct TrackColumnLayout
  {
    std::vector<TrackColumnState> columns;

    auto operator==(TrackColumnLayout const&) const -> bool = default;
  };

  std::optional<TrackColumn> redundantFieldToColumn(ao::app::TrackSortField field);

  std::span<TrackColumnDefinition const> trackColumnDefinitions();

  std::optional<TrackColumn> trackColumnFromId(std::string_view id);

  std::string_view trackColumnId(TrackColumn column);

  TrackColumnLayout defaultTrackColumnLayout();

  TrackColumnLayout normalizeTrackColumnLayout(TrackColumnLayout const& layout);

  class TrackColumnLayoutModel final
  {
  public:
    using ChangedSignal = sigc::signal<void()>;

    explicit TrackColumnLayoutModel(TrackColumnLayout const& layout = defaultTrackColumnLayout());

    TrackColumnLayout const& layout() const { return _layout; }
    void setLayout(TrackColumnLayout const& layout);
    void reset();

    ChangedSignal& signalChanged() { return _changed; }

  private:
    TrackColumnLayout _layout;
    ChangedSignal _changed;
  };
} // namespace ao::gtk
