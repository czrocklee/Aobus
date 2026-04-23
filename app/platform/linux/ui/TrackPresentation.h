// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/Type.h>

#include <sigc++/sigc++.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace app::ui
{

  enum class TrackGroupBy : std::uint8_t
  {
    None,
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Year,
  };

  enum class TrackSortField : std::uint8_t
  {
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Year,
    DiscNumber,
    TrackNumber,
    Title,
    Duration,
  };

  enum class TrackColumn : std::uint8_t
  {
    Artist,
    Album,
    AlbumArtist,
    Genre,
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
    int defaultWidth = -1;
    bool defaultVisible = true;
    bool expands = false;
    bool numeric = false;
    bool tagsCell = false;
  };

  struct TrackColumnState
  {
    TrackColumn column = TrackColumn::Title;
    bool visible = true;
    int width = -1;

    auto operator==(TrackColumnState const&) const -> bool = default;
  };

  struct TrackColumnLayout
  {
    std::vector<TrackColumnState> columns;

    auto operator==(TrackColumnLayout const&) const -> bool = default;
  };

  struct TrackSortTerm
  {
    TrackSortField field;

    auto operator==(TrackSortTerm const&) const -> bool = default;
  };

  struct TrackPresentationSpec
  {
    TrackGroupBy groupBy = TrackGroupBy::None;
    std::vector<TrackSortTerm> sortBy;
  };

  struct TrackPresentationKeysView
  {
    std::string_view artist{};
    std::string_view album{};
    std::string_view albumArtist{};
    std::string_view genre{};
    std::string_view title{};
    std::uint32_t durationMs = 0;
    std::uint16_t year = 0;
    std::uint16_t discNumber = 0;
    std::uint16_t trackNumber = 0;
    rs::core::TrackId trackId{};
  };

  TrackPresentationSpec presentationSpecForGroup(TrackGroupBy groupBy);

  int compareForSort(TrackPresentationKeysView lhs,
                     TrackPresentationKeysView rhs,
                     std::span<TrackSortTerm const> sortBy);

  int compareForGrouping(TrackPresentationKeysView lhs, TrackPresentationKeysView rhs, TrackGroupBy groupBy);

  bool shouldShowColumn(TrackGroupBy groupBy, TrackColumn column);

  std::span<TrackColumnDefinition const> trackColumnDefinitions();

  std::optional<TrackColumn> trackColumnFromId(std::string_view id);

  std::string_view trackColumnId(TrackColumn column);

  TrackColumnLayout defaultTrackColumnLayout();

  TrackColumnLayout normalizeTrackColumnLayout(TrackColumnLayout layout);

  std::string groupLabelFor(TrackPresentationKeysView keys, TrackGroupBy groupBy);

  class TrackColumnLayoutModel final
  {
  public:
    using ChangedSignal = sigc::signal<void()>;

    explicit TrackColumnLayoutModel(TrackColumnLayout layout = defaultTrackColumnLayout());

    TrackColumnLayout const& layout() const { return _layout; }
    void setLayout(TrackColumnLayout layout);
    void reset();

    ChangedSignal& signalChanged() { return _changed; }

  private:
    TrackColumnLayout _layout;
    ChangedSignal _changed;
  };

} // namespace app::ui
