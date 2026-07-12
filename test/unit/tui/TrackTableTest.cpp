// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackTable.h"

#include "test/unit/tui/TuiRenderTestSupport.h"
#include "tui/TrackListEntry.h"
#include "tui/TrackSection.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    std::string lineContaining(std::string_view text, std::string_view needle)
    {
      auto const position = text.find(needle);

      if (position == std::string_view::npos)
      {
        return {};
      }

      auto const lineBegin = text.rfind('\n', position);
      auto const lineEnd = text.find('\n', position);
      auto const begin = lineBegin == std::string_view::npos ? 0 : lineBegin + 1;
      auto const end = lineEnd == std::string_view::npos ? text.size() : lineEnd;
      return std::string{text.substr(begin, end - begin)};
    }

    std::int32_t cellPosition(std::string_view line, std::string_view needle)
    {
      auto const position = line.find(needle);

      if (position == std::string_view::npos)
      {
        return -1;
      }

      return static_cast<std::int32_t>(ftxui::string_width(std::string{line.substr(0, position)}));
    }

    TrackListEntry trackEntry(TrackId const id,
                              std::string title,
                              std::string artist,
                              std::string album,
                              std::uint16_t const trackNumber,
                              std::chrono::milliseconds duration)
    {
      return makeTrackListEntry(rt::TrackRow{.id = id,
                                             .title = std::move(title),
                                             .artist = std::move(artist),
                                             .album = std::move(album),
                                             .duration = duration,
                                             .trackNumber = trackNumber});
    }

    std::vector<TrackListEntry> manyTracks(std::size_t const count)
    {
      auto tracks = std::vector<TrackListEntry>{};
      tracks.reserve(count);

      for (std::size_t index = 0; index < count; ++index)
      {
        tracks.push_back(trackEntry(TrackId{static_cast<std::uint32_t>(index + 1)},
                                    std::format("Track {:05}", index),
                                    "Artist",
                                    "Album",
                                    static_cast<std::uint16_t>((index % 99) + 1),
                                    std::chrono::seconds{60}));
      }

      return tracks;
    }

    // Sectioned list of `sectionCount` equal-sized groups covering `perSection`
    // tracks each, matching the projection's contiguous row-range convention.
    std::vector<TrackSection> equalSections(std::size_t const sectionCount, std::size_t const perSection)
    {
      auto sections = std::vector<TrackSection>{};
      sections.reserve(sectionCount);

      for (std::size_t index = 0; index < sectionCount; ++index)
      {
        sections.push_back(TrackSection{
          .rowBegin = index * perSection, .rowCount = perSection, .primaryText = std::format("Album {:03}", index)});
      }

      return sections;
    }

    // Reference implementation of the header interleaving used by trackTableView
    // before virtualization, against which enumerateTrackTableRows is checked.
    std::vector<TrackTableRowRef> bruteForceTrackTableRows(std::span<TrackSection const> const sections,
                                                           std::size_t const trackCount)
    {
      auto rows = std::vector<TrackTableRowRef>{};
      std::size_t sectionIndex = 0;

      for (std::size_t trackIndex = 0; trackIndex < trackCount; ++trackIndex)
      {
        while (sectionIndex < sections.size() && sections[sectionIndex].rowBegin <= trackIndex)
        {
          rows.push_back(TrackTableRowRef{.isSectionHeader = true, .sectionIndex = sectionIndex});
          ++sectionIndex;
        }

        rows.push_back(TrackTableRowRef{.isSectionHeader = false, .trackIndex = trackIndex});
      }

      return rows;
    }

    bool sameRowRef(TrackTableRowRef const& lhs, TrackTableRowRef const& rhs)
    {
      if (lhs.isSectionHeader != rhs.isSectionHeader)
      {
        return false;
      }

      return lhs.isSectionHeader ? lhs.sectionIndex == rhs.sectionIndex : lhs.trackIndex == rhs.trackIndex;
    }
  } // namespace

  TEST_CASE("TrackTable - track rows keep metadata columns aligned", "[tui][unit][track-table]")
  {
    auto const tracks = std::vector{
      trackEntry(TrackId{1}, "Alpha", "Artist One", "Album One", 7, std::chrono::seconds{65}),
      trackEntry(TrackId{2}, "Beta", "Artist Two", "Album Two", 8, std::chrono::seconds{125}),
    };

    auto const text = renderText(trackTableView(tracks, -1, TrackId{2}, rt::defaultTrackPresentationSpec()), 180);
    auto const header = lineContaining(text, "Title");
    auto const first = lineContaining(text, "Alpha");
    auto const second = lineContaining(text, "Beta");

    REQUIRE_FALSE(header.empty());
    REQUIRE_FALSE(first.empty());
    REQUIRE_FALSE(second.empty());

    CHECK(cellPosition(first, "Artist One") == cellPosition(header, "Artist"));
    CHECK(cellPosition(second, "Artist Two") == cellPosition(header, "Artist"));
    CHECK(cellPosition(first, "Album One") == cellPosition(header, "Album"));
    CHECK(cellPosition(second, "Album Two") == cellPosition(header, "Album"));
    CHECK(cellPosition(first, "1:05") + ftxui::string_width("1:05") ==
          cellPosition(header, "Duration") + ftxui::string_width("Duration"));
    CHECK(cellPosition(second, "2:05") + ftxui::string_width("2:05") ==
          cellPosition(header, "Duration") + ftxui::string_width("Duration"));
    CHECK(first.find('|') == header.find('|'));
    CHECK(second.find('|') == header.find('|'));
  }

  TEST_CASE("TrackTable - wide glyph titles do not shift metadata columns", "[tui][unit][track-table]")
  {
    auto const tracks = std::vector{
      trackEntry(
        TrackId{1}, "今日から思い出（Live in church ver.）", "Aimer", "After Dark", 8, std::chrono::seconds{376}),
      trackEntry(TrackId{2}, "ASCII title", "Artist Two", "Album Two", 9, std::chrono::seconds{125}),
    };

    auto const text = renderText(trackTableView(tracks, -1, kInvalidTrackId, rt::defaultTrackPresentationSpec()), 180);
    auto const header = lineContaining(text, "Title");
    auto const first = lineContaining(text, "今日から思い出");
    auto const second = lineContaining(text, "ASCII title");

    REQUIRE_FALSE(header.empty());
    REQUIRE_FALSE(first.empty());
    REQUIRE_FALSE(second.empty());

    CHECK(cellPosition(first, "Aimer") == cellPosition(header, "Artist"));
    CHECK(cellPosition(second, "Artist Two") == cellPosition(header, "Artist"));
    CHECK(cellPosition(first, "After Dark") == cellPosition(header, "Album"));
    CHECK(cellPosition(second, "Album Two") == cellPosition(header, "Album"));
  }

  TEST_CASE("TrackTable - playing marker uses its own leading column", "[tui][unit][track-table]")
  {
    auto const tracks = std::vector{
      trackEntry(TrackId{1}, "Alpha", "Artist One", "Album One", 7, std::chrono::seconds{65}),
      trackEntry(TrackId{2}, "Beta", "Artist Two", "Album Two", 8, std::chrono::seconds{125}),
    };

    auto const text = renderText(trackTableView(tracks, -1, TrackId{2}, rt::defaultTrackPresentationSpec()));
    auto const first = lineContaining(text, "Alpha");
    auto const second = lineContaining(text, "Beta");

    REQUIRE_FALSE(first.empty());
    REQUIRE_FALSE(second.empty());

    CHECK_FALSE(first.contains('>'));
    CHECK(second.find('>') == 0);
    CHECK(first.find('7') == second.find('8'));
  }

  TEST_CASE("TrackTable - empty state remains visible", "[tui][unit][track-table]")
  {
    auto const tracks = std::vector<TrackListEntry>{};
    auto const text = renderText(trackTableView(tracks, 0, kInvalidTrackId, rt::defaultTrackPresentationSpec()));

    CHECK(text.contains("Title"));
    CHECK(text.contains("No tracks found. Run `aobus init` in this library first."));
  }

  TEST_CASE("TrackTable - presentation controls visible columns", "[tui][unit][track-table]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "tui-test", .visibleFields = {rt::TrackField::Title, rt::TrackField::Year, rt::TrackField::Duration}};
    auto row = rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .duration = std::chrono::seconds{65}, .year = 2026};
    auto const tracks = std::vector{makeTrackListEntry(row)};

    auto const text = renderText(trackTableView(tracks, -1, kInvalidTrackId, presentation));

    CHECK(text.contains("Title"));
    CHECK(text.contains("Year"));
    CHECK(text.contains("Duration"));
    CHECK_FALSE(text.contains("Artist"));
    CHECK_FALSE(text.contains("Album"));
    CHECK(text.contains("2026"));
    CHECK(text.contains("1:05"));
  }

  TEST_CASE("TrackTable - maps track rows through section headers", "[tui][unit][track-table]")
  {
    auto const sections = std::vector{
      TrackSection{.rowBegin = 0, .rowCount = 2, .primaryText = "Album A"},
      TrackSection{.rowBegin = 2, .rowCount = 1, .primaryText = "Album B"},
    };

    CHECK(trackVisualRow(0, sections) == 1);
    CHECK(trackVisualRow(-1, sections) == -1);
    CHECK(trackVisualRow(1, sections) == 2);
    CHECK(trackVisualRow(2, sections) == 4);
    CHECK(trackIndexForVisualRow(0, 3, sections) == 0);
    CHECK(trackIndexForVisualRow(1, 3, sections) == 0);
    CHECK(trackIndexForVisualRow(2, 3, sections) == 1);
    CHECK(trackIndexForVisualRow(3, 3, sections) == 2);
    CHECK(trackIndexForVisualRow(4, 3, sections) == 2);
  }

  TEST_CASE("TrackTable - grouped sections render as non-selectable headers", "[tui][unit][track-table]")
  {
    auto const presentation =
      rt::TrackPresentationSpec{.id = "grouped", .visibleFields = {rt::TrackField::Title, rt::TrackField::Duration}};
    auto const tracks = std::vector{
      makeTrackListEntry(rt::TrackRow{.id = TrackId{1}, .title = "A One", .duration = std::chrono::seconds{61}}),
      makeTrackListEntry(rt::TrackRow{.id = TrackId{2}, .title = "A Two", .duration = std::chrono::seconds{62}}),
      makeTrackListEntry(rt::TrackRow{.id = TrackId{3}, .title = "B One", .duration = std::chrono::seconds{63}}),
    };
    auto const sections = std::vector{
      TrackSection{.rowBegin = 0, .rowCount = 2, .primaryText = "Album A", .secondaryText = "Artist"},
      TrackSection{.rowBegin = 2, .rowCount = 1, .primaryText = "Album B", .secondaryText = "Artist"},
    };
    auto sectionHitRegions = std::vector<TrackSectionRowHitRegion>{};

    auto const rendered =
      renderElement(trackTableView(tracks,
                                   sections,
                                   2,
                                   kInvalidTrackId,
                                   presentation,
                                   TrackTableViewOptions{.sectionRowHitRegions = &sectionHitRegions}),
                    100);
    auto const albumALine = lineIndexContaining(rendered.text, "Album A");
    auto const firstTrackLine = lineIndexContaining(rendered.text, "A One");
    auto const albumBLine = lineIndexContaining(rendered.text, "Album B");
    auto const selectedTrackLine = lineIndexContaining(rendered.text, "B One");

    REQUIRE(albumALine >= 0);
    REQUIRE(firstTrackLine >= 0);
    REQUIRE(albumBLine >= 0);
    REQUIRE(selectedTrackLine >= 0);
    CHECK(albumALine < firstTrackLine);
    CHECK(albumBLine < selectedTrackLine);
    CHECK_FALSE(rendered.screen.PixelAt(2, albumBLine).inverted);
    CHECK_FALSE(rendered.screen.PixelAt(2, selectedTrackLine).inverted);
    checkInteractiveSurface(rendered.screen.PixelAt(2, selectedTrackLine));
    REQUIRE(sectionHitRegions.size() == 2);
    CHECK(sectionHitRegions[1].sectionIndex == 1);
    CHECK(sectionHitRegions[1].box.y_min == albumBLine);
  }

  TEST_CASE("TrackTable - negative selection does not highlight section headers", "[tui][regression][track-table]")
  {
    auto const presentation =
      rt::TrackPresentationSpec{.id = "grouped", .visibleFields = {rt::TrackField::Title, rt::TrackField::Duration}};
    auto const tracks = std::vector{
      makeTrackListEntry(rt::TrackRow{.id = TrackId{1}, .title = "A One", .duration = std::chrono::seconds{61}})};
    auto const sections = std::vector{TrackSection{.rowBegin = 0, .rowCount = 1, .primaryText = "Album A"}};

    auto const rendered = renderElement(trackTableView(tracks, sections, -1, kInvalidTrackId, presentation), 100);
    auto const sectionLine = lineIndexContaining(rendered.text, "Album A");
    auto const trackLine = lineIndexContaining(rendered.text, "A One");

    REQUIRE(sectionLine >= 0);
    REQUIRE(trackLine >= 0);
    CHECK_FALSE(rendered.screen.PixelAt(2, sectionLine).inverted);
    CHECK_FALSE(rendered.screen.PixelAt(2, trackLine).inverted);
  }

  TEST_CASE("TrackTable - column width overrides reposition following columns", "[tui][unit][track-table]")
  {
    auto const presentation =
      rt::TrackPresentationSpec{.id = "widths", .visibleFields = {rt::TrackField::Title, rt::TrackField::Duration}};
    auto const tracks = std::vector{
      makeTrackListEntry(rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .duration = std::chrono::seconds{65}})};
    auto const defaultText = renderText(trackTableView(tracks, -1, kInvalidTrackId, presentation), 120);
    auto const defaultHeader = lineContaining(defaultText, "Duration");
    auto const overrides = std::vector{TrackColumnWidthOverride{.field = rt::TrackField::Title, .columns = 12}};

    auto const resizedText = renderText(
      trackTableView(tracks, -1, kInvalidTrackId, presentation, TrackTableViewOptions{.columnWidths = &overrides}),
      120);
    auto const resizedHeader = lineContaining(resizedText, "Duration");

    REQUIRE_FALSE(defaultHeader.empty());
    REQUIRE_FALSE(resizedHeader.empty());
    CHECK(cellPosition(resizedHeader, "Duration") < cellPosition(defaultHeader, "Duration"));
  }

  TEST_CASE("TrackTable - available terminal width expands flexible columns", "[tui][unit][track-table]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "elastic-widths", .visibleFields = {rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album}};
    auto const tracks = std::vector{
      makeTrackListEntry(rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .artist = "Artist", .album = "Album"})};
    auto narrowHandles = std::vector<TrackColumnResizeHandle>{};
    auto wideHandles = std::vector<TrackColumnResizeHandle>{};

    auto const narrowRendered =
      renderElement(trackTableView(tracks,
                                   -1,
                                   kInvalidTrackId,
                                   presentation,
                                   TrackTableViewOptions{.resizeHandles = &narrowHandles, .availableColumns = 100}),
                    100);
    auto const wideRendered =
      renderElement(trackTableView(tracks,
                                   -1,
                                   kInvalidTrackId,
                                   presentation,
                                   TrackTableViewOptions{.resizeHandles = &wideHandles, .availableColumns = 120}),
                    120);

    REQUIRE_FALSE(narrowRendered.text.empty());
    REQUIRE_FALSE(wideRendered.text.empty());
    REQUIRE(narrowHandles.size() == 3);
    REQUIRE(wideHandles.size() == 3);
    CHECK(wideHandles[0].columns > narrowHandles[0].columns);
    CHECK(wideHandles[1].columns > narrowHandles[1].columns);
    CHECK(wideHandles[2].columns > narrowHandles[2].columns);
  }

  TEST_CASE("TrackTable - column width overrides stay fixed while other text columns flex", "[tui][unit][track-table]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "override-elastic-widths",
      .visibleFields = {rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album}};
    auto const tracks = std::vector{
      makeTrackListEntry(rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .artist = "Artist", .album = "Album"})};
    auto const overrides = std::vector{TrackColumnWidthOverride{.field = rt::TrackField::Title, .columns = 12}};
    auto narrowHandles = std::vector<TrackColumnResizeHandle>{};
    auto wideHandles = std::vector<TrackColumnResizeHandle>{};

    auto const narrowRendered = renderElement(
      trackTableView(
        tracks,
        -1,
        kInvalidTrackId,
        presentation,
        TrackTableViewOptions{.columnWidths = &overrides, .resizeHandles = &narrowHandles, .availableColumns = 100}),
      100);
    auto const wideRendered = renderElement(
      trackTableView(
        tracks,
        -1,
        kInvalidTrackId,
        presentation,
        TrackTableViewOptions{.columnWidths = &overrides, .resizeHandles = &wideHandles, .availableColumns = 120}),
      120);

    REQUIRE_FALSE(narrowRendered.text.empty());
    REQUIRE_FALSE(wideRendered.text.empty());
    REQUIRE(narrowHandles.size() == 3);
    REQUIRE(wideHandles.size() == 3);
    CHECK(narrowHandles[0].columns == 12);
    CHECK(wideHandles[0].columns == 12);
    CHECK(wideHandles[1].columns > narrowHandles[1].columns);
    CHECK(wideHandles[2].columns > narrowHandles[2].columns);
  }

  TEST_CASE("TrackTable - narrow available width clamps columns to terminal minimum", "[tui][unit][track-table]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "narrow-elastic-widths",
      .visibleFields = {rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album}};
    auto const tracks = std::vector{
      makeTrackListEntry(rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .artist = "Artist", .album = "Album"})};
    auto handles = std::vector<TrackColumnResizeHandle>{};

    auto const rendered =
      renderElement(trackTableView(tracks,
                                   -1,
                                   kInvalidTrackId,
                                   presentation,
                                   TrackTableViewOptions{.resizeHandles = &handles, .availableColumns = 30}),
                    40);

    REQUIRE_FALSE(rendered.text.empty());
    REQUIRE(handles.size() == 3);
    CHECK(handles[0].columns == kMinimumTrackColumnWidthColumns);
    CHECK(handles[1].columns == kMinimumTrackColumnWidthColumns);
    CHECK(handles[2].columns == kMinimumTrackColumnWidthColumns);
  }

  TEST_CASE("TrackTable - header exposes resize handles", "[tui][unit][track-table]")
  {
    auto const presentation =
      rt::TrackPresentationSpec{.id = "handles", .visibleFields = {rt::TrackField::Title, rt::TrackField::Duration}};
    auto const tracks = std::vector{
      makeTrackListEntry(rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .duration = std::chrono::seconds{65}})};
    auto handles = std::vector<TrackColumnResizeHandle>{};

    auto const rendered = renderElement(
      trackTableView(tracks, -1, kInvalidTrackId, presentation, TrackTableViewOptions{.resizeHandles = &handles}), 120);

    REQUIRE_FALSE(rendered.text.empty());
    REQUIRE(handles.size() == 2);
    auto const header = lineContaining(rendered.text, "Title");
    REQUIRE(static_cast<std::size_t>(handles[0].box.x_max + 1) < header.size());
    CHECK(handles[0].field == rt::TrackField::Title);
    CHECK(handles[0].columns > 0);
    CHECK(handles[0].box.x_min < handles[0].box.x_max);
    CHECK(handles[0].box.y_min == handles[0].box.y_max);
    CHECK(header[static_cast<std::size_t>(handles[0].box.x_max + 1)] == '|');
  }

  TEST_CASE("TrackTable - selected row style fills the table width", "[tui][unit][track-table]")
  {
    auto const tracks = std::vector{
      trackEntry(TrackId{1}, "Alpha", "Artist One", "Album One", 7, std::chrono::seconds{65}),
      trackEntry(TrackId{2}, "Beta", "Artist Two", "Album Two", 8, std::chrono::seconds{125}),
    };

    auto const rendered = renderElement(trackTableView(tracks, 1, TrackId{2}, rt::defaultTrackPresentationSpec()), 96);
    auto const row = lineIndexContaining(rendered.text, "Beta");

    REQUIRE(row >= 0);
    CHECK_FALSE(rendered.screen.PixelAt(2, row).inverted);
    checkInteractiveSurface(rendered.screen.PixelAt(2, row));
    CHECK_FALSE(rendered.screen.PixelAt(90, row).inverted);
    checkInteractiveSurface(rendered.screen.PixelAt(90, row));
  }

  TEST_CASE("TrackTable - selected row is scrolled into a short viewport", "[tui][unit][track-table]")
  {
    auto tracks = std::vector<TrackListEntry>{};

    for (std::int32_t index = 0; index < 30; ++index)
    {
      tracks.push_back(trackEntry(TrackId{static_cast<std::uint32_t>(index + 1)},
                                  std::format("Track {:02}", index),
                                  "Artist",
                                  "Album",
                                  static_cast<std::uint16_t>(index + 1),
                                  std::chrono::seconds{60}));
    }

    auto const presentation = rt::TrackPresentationSpec{
      .id = "short", .visibleFields = {rt::TrackField::DisplayTrackNumber, rt::TrackField::Title}};
    auto const rendered = renderElement(trackTableView(tracks, 24, kInvalidTrackId, presentation), 48, 7);

    CHECK(rendered.text.contains("Track 24"));
    CHECK_FALSE(rendered.text.contains("Track 00"));
  }

  TEST_CASE("TrackTable - empty field fallbacks are visible", "[tui][unit][track-table]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "fallbacks",
      .visibleFields = {
        rt::TrackField::DisplayTrackNumber, rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Duration}};
    auto const tracks = std::vector{makeTrackListEntry(rt::TrackRow{.id = TrackId{9}})};

    auto const text = renderText(trackTableView(tracks, -1, kInvalidTrackId, presentation));
    auto const header = lineContaining(text, "Title");
    auto const row = lineContaining(text, "Track 9");

    REQUIRE_FALSE(header.empty());
    REQUIRE_FALSE(row.empty());

    auto const trackNumberColumn = header.find("Track #");
    auto const artistColumn = header.find("Artist");
    REQUIRE(trackNumberColumn != std::string::npos);
    REQUIRE(artistColumn != std::string::npos);

    CHECK(row.find("--") == trackNumberColumn + std::string_view{"Track #"}.size() - std::string_view{"--"}.size());
    CHECK(row.at(artistColumn) == '-');
    CHECK(row.contains("--:--"));
  }

  TEST_CASE("TrackTable - library chooser width follows labels and terminal bounds", "[tui][unit][track-table]")
  {
    auto labels = std::vector<std::string>{"All Tracks", "[?] Very Long Smart List Name For Testing"};
    auto const wideColumns = libraryChooserPaneColumns(labels, 120);

    CHECK(wideColumns > libraryChooserPaneColumns({"All Tracks"}, 120));
    CHECK(wideColumns <= 120);
    CHECK(libraryChooserPaneColumns(labels, 24) == 24);

    auto const text = renderText(libraryChooserPane(labels, 1, wideColumns), wideColumns);

    CHECK(text.contains("Very Long Smart List"));
  }

  TEST_CASE("TrackTable - title column expands on wide terminals", "[tui][unit][track-table]")
  {
    auto const presentation =
      rt::TrackPresentationSpec{.id = "wide", .visibleFields = {rt::TrackField::Title, rt::TrackField::Duration}};
    auto row = rt::TrackRow{
      .id = TrackId{1},
      .title = "A very long title that should still reveal the wide-terminal-tail marker",
      .duration = std::chrono::seconds{65},
    };
    auto const tracks = std::vector{makeTrackListEntry(row)};

    auto const text = renderText(
      trackTableView(tracks, -1, kInvalidTrackId, presentation, TrackTableViewOptions{.availableColumns = 140}), 140);

    CHECK(text.contains("wide-terminal-tail"));
  }

  TEST_CASE("TrackTable - computeTrackTableWindow disables windowing without a viewport", "[tui][unit][track-table]")
  {
    auto const window = computeTrackTableWindow(50, 100, 0, kTrackTableOverscanRows);

    CHECK(window.startVisualRow == 0);
    CHECK(window.endVisualRow == 100);
    CHECK(window.topSpacerRows == 0);
    CHECK(window.bottomSpacerRows == 0);
  }

  TEST_CASE("TrackTable - computeTrackTableWindow keeps spacer sums equal to the total", "[tui][unit][track-table]")
  {
    std::int32_t const overscan = 8;
    std::int32_t const total = 1000;
    std::int32_t const viewport = 40;
    auto const half = viewport + overscan;

    auto const check = [&](std::int32_t const selected)
    {
      auto const window = computeTrackTableWindow(selected, total, viewport, overscan);

      CHECK(window.topSpacerRows == window.startVisualRow);
      CHECK(window.bottomSpacerRows == total - window.endVisualRow);
      CHECK(window.startVisualRow >= 0);
      CHECK(window.endVisualRow <= total);
      CHECK(window.startVisualRow < window.endVisualRow);
      CHECK(window.topSpacerRows + (window.endVisualRow - window.startVisualRow) + window.bottomSpacerRows == total);
      CHECK(window.endVisualRow - window.startVisualRow <= (2 * half) + 1);
      return window;
    };

    auto const middle = check(500);
    CHECK(middle.startVisualRow == 500 - half);
    CHECK(middle.endVisualRow == 500 + half + 1);

    auto const top = check(10);
    CHECK(top.startVisualRow == 0);
    CHECK(top.topSpacerRows == 0);

    auto const bottom = check(990);
    CHECK(bottom.endVisualRow == total);
    CHECK(bottom.bottomSpacerRows == 0);

    // selected == -1 anchors at row 0, matching focusPosition(0, max(0, selected)).
    auto const negative = check(-1);
    CHECK(negative.startVisualRow == 0);
  }

  TEST_CASE("TrackTable - computeTrackTableWindow drops both spacers for short lists", "[tui][unit][track-table]")
  {
    auto const window = computeTrackTableWindow(25, 50, 40, 8);

    CHECK(window.startVisualRow == 0);
    CHECK(window.endVisualRow == 50);
    CHECK(window.topSpacerRows == 0);
    CHECK(window.bottomSpacerRows == 0);
  }

  TEST_CASE("TrackTable - computeTrackTableWindow returns empty for an empty list", "[tui][unit][track-table]")
  {
    auto const window = computeTrackTableWindow(0, 0, 40, 8);

    CHECK(window.startVisualRow == 0);
    CHECK(window.endVisualRow == 0);
    CHECK(window.topSpacerRows == 0);
    CHECK(window.bottomSpacerRows == 0);
  }

  TEST_CASE("TrackTable - enumerateTrackTableRows reproduces the full build across windows", "[tui][unit][track-table]")
  {
    auto const noSections = std::vector<TrackSection>{};
    auto const grouped = equalSections(6, 5); // 30 tracks, headers at 0/5/10/15/20/25

    auto const check = [](std::span<TrackSection const> const sections, std::size_t const trackCount)
    {
      auto const full = bruteForceTrackTableRows(sections, trackCount);
      auto const totalVisualRows = static_cast<std::int32_t>(full.size());

      for (std::int32_t start = 0; start <= totalVisualRows; ++start)
      {
        for (std::int32_t end = start; end <= totalVisualRows + 2; ++end)
        {
          auto const windowed = enumerateTrackTableRows(sections, trackCount, start, end);
          auto const clampedEnd = std::min(end, totalVisualRows);
          auto const expectedSize = static_cast<std::size_t>(std::max(0, clampedEnd - start));

          REQUIRE(windowed.size() == expectedSize);

          for (std::size_t index = 0; index < windowed.size(); ++index)
          {
            CHECK(sameRowRef(windowed[index], full[static_cast<std::size_t>(start) + index]));
          }
        }
      }
    };

    check(noSections, 20);
    check(grouped, 30);
  }

  TEST_CASE("TrackTable - enumerateTrackTableRows honors window boundaries", "[tui][unit][track-table]")
  {
    auto const sections = std::vector{
      TrackSection{.rowBegin = 0, .rowCount = 2, .primaryText = "Album A"},
      TrackSection{.rowBegin = 2, .rowCount = 1, .primaryText = "Album B"},
    };
    // Visual rows: 0=header A, 1=track 0, 2=track 1, 3=header B, 4=track 2.
    SECTION("window starting on a header emits the header then its track")
    {
      auto const rows = enumerateTrackTableRows(sections, 3, 3, 5);

      REQUIRE(rows.size() == 2);
      CHECK(rows[0].isSectionHeader);
      CHECK(rows[0].sectionIndex == 1);
      CHECK_FALSE(rows[1].isSectionHeader);
      CHECK(rows[1].trackIndex == 2);
    }

    SECTION("window starting mid-section omits the enclosing header")
    {
      auto const rows = enumerateTrackTableRows(sections, 3, 2, 3);

      REQUIRE(rows.size() == 1);
      CHECK_FALSE(rows[0].isSectionHeader);
      CHECK(rows[0].trackIndex == 1);
    }
  }

  TEST_CASE("TrackTable - virtualized render is pixel-identical to the full build", "[tui][unit][track-table]")
  {
    auto const tracks = manyTracks(5000);
    auto const presentation = rt::TrackPresentationSpec{
      .id = "virtualized", .visibleFields = {rt::TrackField::DisplayTrackNumber, rt::TrackField::Title}};
    std::int32_t const width = 80;
    std::int32_t const height = 30;

    auto const render = [&](std::int32_t const selected, std::int32_t const viewportRows)
    {
      return renderElement(
        trackTableView(tracks,
                       selected,
                       kInvalidTrackId,
                       presentation,
                       TrackTableViewOptions{.availableColumns = width, .viewportRows = viewportRows}),
        width,
        height);
    };

    for (auto const selected : {0, 2500, 4999})
    {
      auto const full = render(selected, 0);
      auto const windowed = render(selected, height);

      CHECK(full.text == windowed.text);
      CHECK(full.screen.ToString() == windowed.screen.ToString());
      CHECK(windowed.text.contains(std::format("Track {:05}", selected)));
    }
  }

  TEST_CASE("TrackTable - virtualized grouped render matches the full build", "[tui][unit][track-table]")
  {
    auto const tracks = manyTracks(300);
    auto const sections = equalSections(10, 30);
    auto const presentation = rt::TrackPresentationSpec{
      .id = "virtualized-grouped", .visibleFields = {rt::TrackField::DisplayTrackNumber, rt::TrackField::Title}};
    std::int32_t const width = 80;
    std::int32_t const height = 24;
    std::int32_t const selected = 150; // deep in section 5

    auto const render = [&](std::int32_t const viewportRows)
    {
      return renderElement(
        trackTableView(tracks,
                       sections,
                       selected,
                       kInvalidTrackId,
                       presentation,
                       TrackTableViewOptions{.availableColumns = width, .viewportRows = viewportRows}),
        width,
        height);
    };

    auto const full = render(0);
    auto hitRegions = std::vector<TrackSectionRowHitRegion>{};
    auto const windowed = renderElement(
      trackTableView(
        tracks,
        sections,
        selected,
        kInvalidTrackId,
        presentation,
        TrackTableViewOptions{.sectionRowHitRegions = &hitRegions, .availableColumns = width, .viewportRows = height}),
      width,
      height);

    CHECK(full.text == windowed.text);
    CHECK(full.screen.ToString() == windowed.screen.ToString());
    // The enclosing section header for the selected row stays visible.
    auto const sectionLine = lineIndexContaining(windowed.text, "Album 005");
    REQUIRE(sectionLine >= 0);
    CHECK(windowed.text.contains(std::format("Track {:05}", selected)));

    auto const sectionRegion = std::ranges::find(hitRegions, 5, &TrackSectionRowHitRegion::sectionIndex);
    REQUIRE(sectionRegion != hitRegions.end());
    CHECK(sectionRegion->box.y_min == sectionLine);
    CHECK(sectionRegion->box.y_max == sectionLine);
  }

  TEST_CASE("TrackTable - virtualized render without selection matches the full build", "[tui][unit][track-table]")
  {
    auto const tracks = manyTracks(4000);
    auto const presentation = rt::TrackPresentationSpec{
      .id = "virtualized-noselect", .visibleFields = {rt::TrackField::DisplayTrackNumber, rt::TrackField::Title}};
    std::int32_t const width = 72;
    std::int32_t const height = 20;

    auto const render = [&](std::int32_t const viewportRows)
    {
      return renderElement(
        trackTableView(tracks,
                       -1,
                       kInvalidTrackId,
                       presentation,
                       TrackTableViewOptions{.availableColumns = width, .viewportRows = viewportRows}),
        width,
        height);
    };

    auto const full = render(0);
    auto const windowed = render(height);

    CHECK(full.text == windowed.text);
    CHECK(full.screen.ToString() == windowed.screen.ToString());

    // No row is highlighted when nothing is selected (the clamped window anchor
    // must not leak into the selection highlight).
    for (std::int32_t row = 0; row < windowed.screen.dimy(); ++row)
    {
      CHECK_FALSE(windowed.screen.PixelAt(2, row).inverted);
    }
  }

  TEST_CASE("TrackTable - virtualization builds O(viewport) rows, not the whole library",
            "[tui][regression][track-table]")
  {
    std::size_t const trackCount = 5000;
    std::int32_t const viewportRows = 40;
    auto const totalVisualRows = static_cast<std::int32_t>(trackCount);
    auto const selectedVisualRow = trackVisualRow(2500, {});
    auto const window =
      computeTrackTableWindow(selectedVisualRow, totalVisualRows, viewportRows, kTrackTableOverscanRows);
    auto const rows = enumerateTrackTableRows({}, trackCount, window.startVisualRow, window.endVisualRow);

    CHECK(rows.size() == static_cast<std::size_t>(window.endVisualRow - window.startVisualRow));
    CHECK(rows.size() <= static_cast<std::size_t>((2 * (viewportRows + kTrackTableOverscanRows)) + 1));
    CHECK(rows.size() < trackCount);
  }
} // namespace ao::tui::test
