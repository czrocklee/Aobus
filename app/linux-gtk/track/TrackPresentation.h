// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <runtime/ProjectionTypes.h>
#include <runtime/StateTypes.h>
#include <runtime/TrackPresentationPreset.h>

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

  struct TrackColumnDefinition final
  {
    TrackColumn column;
    std::string_view id;
    std::string_view title;
    std::int32_t defaultWidth = -1;
    bool defaultVisible = true;
    bool expands = false;
    bool numeric = false;
    bool tagsCell = false;
    bool editable = false;
    bool draggable = false;
  };

  struct TrackColumnState final
  {
    TrackColumn column = TrackColumn::Title;
    bool visible = true;
    std::int32_t width = -1;

    bool operator==(TrackColumnState const&) const = default;
  };

  struct TrackColumnLayout final
  {
    std::vector<TrackColumnState> columns;

    bool operator==(TrackColumnLayout const&) const = default;
  };

  std::optional<TrackColumn> redundantFieldToColumn(rt::TrackSortField field);
  std::optional<TrackColumn> trackColumnForPresentationField(rt::TrackPresentationField field);
  TrackColumnLayout trackColumnLayoutForPresentation(rt::TrackPresentationSpec const& presentation);
  TrackColumnLayout trackColumnLayoutForPresentation(rt::TrackListPresentationSnapshot const& presentation);

  std::span<TrackColumnDefinition const> trackColumnDefinitions();
  TrackColumnDefinition const* trackColumnDefinition(TrackColumn column);
  TrackColumnState defaultTrackColumnState(TrackColumn column);

  std::optional<TrackColumn> trackColumnFromId(std::string_view id);

  std::string_view trackColumnId(TrackColumn column);

  TrackColumnLayout defaultTrackColumnLayout();

  TrackColumnLayout normalizeTrackColumnLayout(TrackColumnLayout const& layout);
  std::vector<TrackColumn> expandingTrackColumnsForLayout(TrackColumnLayout const& layout);

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
