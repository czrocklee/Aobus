// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackTable.h"

#include "tui/Model.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    struct RenderedElement final
    {
      ftxui::Screen screen;
      std::string text{};
    };

    std::string stripAnsi(std::string_view text)
    {
      auto result = std::string{};
      result.reserve(text.size());

      for (std::size_t index = 0; index < text.size(); ++index)
      {
        if (text[index] != '\x1B')
        {
          result.push_back(text[index]);
          continue;
        }

        ++index;

        while (index < text.size() && std::isalpha(static_cast<unsigned char>(text[index])) == 0 && text[index] != '\\')
        {
          ++index;
        }
      }

      return result;
    }

    RenderedElement renderElement(ftxui::Element elementPtr,
                                  std::int32_t const width = 120,
                                  std::optional<std::int32_t> const optHeight = {})
    {
      auto screen = optHeight
                      ? ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(*optHeight))
                      : ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fit(elementPtr));
      ftxui::Render(screen, elementPtr);
      auto text = stripAnsi(screen.ToString());
      return RenderedElement{.screen = std::move(screen), .text = std::move(text)};
    }

    std::string renderText(ftxui::Element elementPtr, std::int32_t const width = 120)
    {
      return renderElement(std::move(elementPtr), width).text;
    }

    std::int32_t lineIndexContaining(std::string_view text, std::string_view needle)
    {
      std::int32_t lineIndex = 0;
      std::size_t lineStart = 0;

      while (lineStart < text.size())
      {
        auto const lineEnd = text.find('\n', lineStart);
        auto const end = lineEnd == std::string_view::npos ? text.size() : lineEnd;

        if (auto const line = text.substr(lineStart, end - lineStart); line.find(needle) != std::string_view::npos)
        {
          return lineIndex;
        }

        if (lineEnd == std::string_view::npos)
        {
          break;
        }

        lineStart = lineEnd + 1;
        ++lineIndex;
      }

      return -1;
    }

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

    auto const text = renderText(trackTableView(tracks, -1, TrackId{2}, rt::defaultTrackPresentationSpec()));
    auto const header = lineContaining(text, "Title");
    auto const first = lineContaining(text, "Alpha");
    auto const second = lineContaining(text, "Beta");

    REQUIRE_FALSE(header.empty());
    REQUIRE_FALSE(first.empty());
    REQUIRE_FALSE(second.empty());

    CHECK(first.find("Artist One") == header.find("Artist"));
    CHECK(second.find("Artist Two") == header.find("Artist"));
    CHECK(first.find("Album One") == header.find("Album"));
    CHECK(second.find("Album Two") == header.find("Album"));
    CHECK(first.find("1:05") + std::string_view{"1:05"}.size() ==
          header.find("Duration") + std::string_view{"Duration"}.size());
    CHECK(second.find("2:05") + std::string_view{"2:05"}.size() ==
          header.find("Duration") + std::string_view{"Duration"}.size());
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
    auto const text = renderText(trackTableView({}, 0, kInvalidTrackId, rt::defaultTrackPresentationSpec()));

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

  TEST_CASE("TrackTable - selected row style fills the table width", "[tui][unit][track-table]")
  {
    auto const tracks = std::vector{
      trackItem(TrackId{1}, "Alpha", "Artist One", "Album One", 7, std::chrono::seconds{65}),
      trackItem(TrackId{2}, "Beta", "Artist Two", "Album Two", 8, std::chrono::seconds{125}),
    };

    auto const rendered = renderElement(trackTableView(tracks, 1, TrackId{2}, rt::defaultTrackPresentationSpec()), 96);
    auto const row = lineIndexContaining(rendered.text, "Beta");

    REQUIRE(row >= 0);
    CHECK(rendered.screen.PixelAt(2, row).inverted);
    CHECK(rendered.screen.PixelAt(2, row).bold);
    CHECK(rendered.screen.PixelAt(90, row).inverted);
    CHECK(rendered.screen.PixelAt(90, row).bold);
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

    auto const text = renderText(trackTableView(tracks, -1, kInvalidTrackId, presentation), 140);

    CHECK(text.find("wide-terminal-tail") != std::string::npos);
  }
} // namespace ao::tui::test
