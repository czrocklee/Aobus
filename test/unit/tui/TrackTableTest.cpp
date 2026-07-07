// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackTable.h"

#include "test/unit/tui/TuiRenderTestSupport.h"
#include "tui/Model.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
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

    TrackListItem trackItem(TrackId const id,
                            std::string title,
                            std::string artist,
                            std::string album,
                            std::uint16_t const trackNumber,
                            std::chrono::milliseconds duration)
    {
      return makeTrackListItem(rt::TrackRow{.id = id,
                                            .title = std::move(title),
                                            .artist = std::move(artist),
                                            .album = std::move(album),
                                            .duration = duration,
                                            .trackNumber = trackNumber});
    }
  } // namespace

  TEST_CASE("TrackTable - track rows keep metadata columns aligned", "[tui][unit][track-table]")
  {
    auto const tracks = std::vector{
      trackItem(TrackId{1}, "Alpha", "Artist One", "Album One", 7, std::chrono::seconds{65}),
      trackItem(TrackId{2}, "Beta", "Artist Two", "Album Two", 8, std::chrono::seconds{125}),
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
      trackItem(
        TrackId{1}, "今日から思い出（Live in church ver.）", "Aimer", "After Dark", 8, std::chrono::seconds{376}),
      trackItem(TrackId{2}, "ASCII title", "Artist Two", "Album Two", 9, std::chrono::seconds{125}),
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
      trackItem(TrackId{1}, "Alpha", "Artist One", "Album One", 7, std::chrono::seconds{65}),
      trackItem(TrackId{2}, "Beta", "Artist Two", "Album Two", 8, std::chrono::seconds{125}),
    };

    auto const text = renderText(trackTableView(tracks, -1, TrackId{2}, rt::defaultTrackPresentationSpec()));
    auto const first = lineContaining(text, "Alpha");
    auto const second = lineContaining(text, "Beta");

    REQUIRE_FALSE(first.empty());
    REQUIRE_FALSE(second.empty());

    CHECK(first.find('>') == std::string::npos);
    CHECK(second.find('>') == 0);
    CHECK(first.find('7') == second.find('8'));
  }

  TEST_CASE("TrackTable - empty state remains visible", "[tui][unit][track-table]")
  {
    auto const tracks = std::vector<TrackListItem>{};
    auto const text = renderText(trackTableView(tracks, 0, kInvalidTrackId, rt::defaultTrackPresentationSpec()));

    CHECK(text.find("Title") != std::string::npos);
    CHECK(text.find("No tracks found. Run `aobus init` in this library first.") != std::string::npos);
  }

  TEST_CASE("TrackTable - presentation controls visible columns", "[tui][unit][track-table]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "tui-test", .visibleFields = {rt::TrackField::Title, rt::TrackField::Year, rt::TrackField::Duration}};
    auto row = rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .duration = std::chrono::seconds{65}, .year = 2026};
    auto const tracks = std::vector{makeTrackListItem(row)};

    auto const text = renderText(trackTableView(tracks, -1, kInvalidTrackId, presentation));

    CHECK(text.find("Title") != std::string::npos);
    CHECK(text.find("Year") != std::string::npos);
    CHECK(text.find("Duration") != std::string::npos);
    CHECK(text.find("Artist") == std::string::npos);
    CHECK(text.find("Album") == std::string::npos);
    CHECK(text.find("2026") != std::string::npos);
    CHECK(text.find("1:05") != std::string::npos);
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
      makeTrackListItem(rt::TrackRow{.id = TrackId{1}, .title = "A One", .duration = std::chrono::seconds{61}}),
      makeTrackListItem(rt::TrackRow{.id = TrackId{2}, .title = "A Two", .duration = std::chrono::seconds{62}}),
      makeTrackListItem(rt::TrackRow{.id = TrackId{3}, .title = "B One", .duration = std::chrono::seconds{63}}),
    };
    auto const sections = std::vector{
      TrackSection{.rowBegin = 0, .rowCount = 2, .primaryText = "Album A", .secondaryText = "Artist"},
      TrackSection{.rowBegin = 2, .rowCount = 1, .primaryText = "Album B", .secondaryText = "Artist"},
    };
    auto sectionBoxes = std::vector<TrackSectionRowBox>{};

    auto const rendered = renderElement(
      trackTableView(
        tracks, sections, 2, kInvalidTrackId, presentation, TrackTableViewOptions{.sectionRowBoxes = &sectionBoxes}),
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
    REQUIRE(sectionBoxes.size() == 2);
    CHECK(sectionBoxes[1].sectionIndex == 1);
    CHECK(sectionBoxes[1].box.y_min == albumBLine);
  }

  TEST_CASE("TrackTable - negative selection does not highlight section headers", "[tui][regression][track-table]")
  {
    auto const presentation =
      rt::TrackPresentationSpec{.id = "grouped", .visibleFields = {rt::TrackField::Title, rt::TrackField::Duration}};
    auto const tracks = std::vector{
      makeTrackListItem(rt::TrackRow{.id = TrackId{1}, .title = "A One", .duration = std::chrono::seconds{61}})};
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
      makeTrackListItem(rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .duration = std::chrono::seconds{65}})};
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
      makeTrackListItem(rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .artist = "Artist", .album = "Album"})};
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
      makeTrackListItem(rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .artist = "Artist", .album = "Album"})};
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
      makeTrackListItem(rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .artist = "Artist", .album = "Album"})};
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
      makeTrackListItem(rt::TrackRow{.id = TrackId{1}, .title = "Alpha", .duration = std::chrono::seconds{65}})};
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
      trackItem(TrackId{1}, "Alpha", "Artist One", "Album One", 7, std::chrono::seconds{65}),
      trackItem(TrackId{2}, "Beta", "Artist Two", "Album Two", 8, std::chrono::seconds{125}),
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
    auto tracks = std::vector<TrackListItem>{};

    for (std::int32_t index = 0; index < 30; ++index)
    {
      tracks.push_back(trackItem(TrackId{static_cast<std::uint32_t>(index + 1)},
                                 std::format("Track {:02}", index),
                                 "Artist",
                                 "Album",
                                 static_cast<std::uint16_t>(index + 1),
                                 std::chrono::seconds{60}));
    }

    auto const presentation = rt::TrackPresentationSpec{
      .id = "short", .visibleFields = {rt::TrackField::DisplayTrackNumber, rt::TrackField::Title}};
    auto const rendered = renderElement(trackTableView(tracks, 24, kInvalidTrackId, presentation), 48, 7);

    CHECK(rendered.text.find("Track 24") != std::string::npos);
    CHECK(rendered.text.find("Track 00") == std::string::npos);
  }

  TEST_CASE("TrackTable - empty field fallbacks are visible", "[tui][unit][track-table]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "fallbacks",
      .visibleFields = {
        rt::TrackField::DisplayTrackNumber, rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Duration}};
    auto const tracks = std::vector{makeTrackListItem(rt::TrackRow{.id = TrackId{9}})};

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
    CHECK(row.find("--:--") != std::string::npos);
  }

  TEST_CASE("TrackTable - library chooser width follows labels and terminal bounds", "[tui][unit][track-table]")
  {
    auto labels = std::vector<std::string>{"All Tracks", "[?] Very Long Smart List Name For Testing"};
    auto const wideColumns = libraryChooserPaneColumns(labels, 120);

    CHECK(wideColumns > libraryChooserPaneColumns({"All Tracks"}, 120));
    CHECK(wideColumns <= 120);
    CHECK(libraryChooserPaneColumns(labels, 24) == 24);

    auto const text = renderText(libraryChooserPane(labels, 1, wideColumns), wideColumns);

    CHECK(text.find("Very Long Smart List") != std::string::npos);
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
    auto const tracks = std::vector{makeTrackListItem(row)};

    auto const text = renderText(
      trackTableView(tracks, -1, kInvalidTrackId, presentation, TrackTableViewOptions{.availableColumns = 140}), 140);

    CHECK(text.find("wide-terminal-tail") != std::string::npos);
  }
} // namespace ao::tui::test
