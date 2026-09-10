// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/Render.h"
#include "tui/TextCell.h"
#include "tui/TrackListEntry.h"
#include <ao/AudioCodec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>

namespace ao::tui::test
{
  TEST_CASE("DetailFieldLayout - metadata audio and tags share a value column across locales",
            "[tui][regression][detail]")
  {
    auto const row = rt::TrackRow{.title = "Alpha\nOmega",
                                  .artist = "Bach",
                                  .album = "Goldberg",
                                  .tags = "fav",
                                  .duration = std::chrono::seconds{299},
                                  .year = 2019,
                                  .trackNumber = 7,
                                  .trackTotal = 12,
                                  .sampleRate = 48000,
                                  .bitDepth = 24,
                                  .codec = AudioCodec::Flac};

    for (std::string_view const locale : {"en-US", "de-DE", "zh-Hans", "zh-Hant", "ja-JP"})
    {
      auto const catalog = ao::test::messageCatalog(locale);
      auto const track = makeTrackListEntry(catalog, row);

      for (auto const columns : {24, 40, 56})
      {
        CAPTURE(locale, columns);
        auto const rendered = renderElement(
          detailPane(catalog, &track, {}, columns, nullptr, 0, {.sections = {.expanded = {true, true}}}), columns, 32);
        INFO(rendered.text);
        auto const optTitle = findTextCells(rendered.screen, "Alpha");
        REQUIRE(optTitle);
        CHECK(optTitle->x_min > 2);
        CHECK(rendered.screen.PixelAt(optTitle->x_min, optTitle->y_min).bold);

        for (std::string_view const value :
             {"Omega", "Bach", "Goldberg", "2019", "7 / 12", "4:59", "FLAC", "48000 Hz", "fav"})
        {
          CAPTURE(value);
          auto const optCells = findTextCells(rendered.screen, value);
          REQUIRE(optCells);
          CHECK(optCells->x_min == optTitle->x_min);
          CHECK(optCells->x_max <= columns - 3);
        }

        auto const titleLabel = uimodel::trackFieldLabel(catalog, rt::TrackField::Title);

        if (auto const labelColumns = optTitle->x_min - 2 - 2; cellWidth(titleLabel) <= labelColumns)
        {
          auto const optLabel = findTextCells(rendered.screen, titleLabel);
          REQUIRE(optLabel);
          CHECK(optLabel->x_min == 2);
          CHECK(optLabel->y_min == optTitle->y_min);
        }

        auto const& labelStart = rendered.screen.PixelAt(2, optTitle->y_min);
        CHECK_FALSE(labelStart.character.empty());
        CHECK(labelStart.character != " ");
        CHECK(labelStart.dim);
        CHECK(rendered.screen.PixelAt(optTitle->x_min - 1, optTitle->y_min).character == " ");
        CHECK(rendered.screen.PixelAt(optTitle->x_min - 2, optTitle->y_min).character == " ");
        CHECK_FALSE(rendered.text.contains("Title:"));
      }
    }
  }

  TEST_CASE("DetailFieldLayout - missing title is omitted but collapsed identity retains its filename",
            "[tui][regression][detail]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto const row = rt::TrackRow{.optUriPath = "/music/untitled.flac"};
    auto const track = makeTrackListEntry(catalog, row);
    auto const expanded = renderElement(detailPane(catalog, &track, {}, 40), 40, 12);
    CHECK_FALSE(expanded.text.contains("Title"));
    CHECK_FALSE(expanded.text.contains("untitled"));
    auto const collapsed = renderElement(
      detailPane(catalog, &track, {}, 40, nullptr, 0, {.sections = {.expanded = {false, false}}}), 40, 12);
    CHECK(collapsed.text.contains("untitled.flac"));
  }

  TEST_CASE("DetailFieldLayout - display controls cannot corrupt wrapped Unicode metadata", "[tui][regression][detail]")
  {
    using namespace std::string_view_literals;
    // NOLINTNEXTLINE(misc-include-cleaner) -- MSVC include-cleaner cannot map sv to the included <string_view>.
    constexpr auto kWomanTechnologist = "\U0001F469\u200D\U0001F4BB"sv;
    auto const title = std::string{"\u0301a\t中\r\n\u0301\u0085文\x7F"
                                   "字"} +
                       std::string{kWomanTechnologist} + " Tail";
    auto const& catalog = ao::test::englishMessageCatalog();
    auto const track = makeTrackListEntry(catalog, rt::TrackRow{.title = title});
    auto const rendered =
      renderElement(detailPane(catalog, &track, {}, 24, nullptr, 0, {.sections = {.expanded = {true, true}}}), 24, 20);
    INFO(rendered.text);
    auto const optFirst = findTextCells(rendered.screen, "a中");
    auto const optChinese = findTextCells(rendered.screen, "文字");
    auto const optEmoji = findTextCells(rendered.screen, kWomanTechnologist);
    auto const optTail = findTextCells(rendered.screen, "Tail");
    REQUIRE(optFirst);
    REQUIRE(optChinese);
    REQUIRE(optEmoji);
    REQUIRE(optTail);
    CHECK(optFirst->x_min == optChinese->x_min);
    CHECK(optFirst->x_min == optTail->x_min);
    CHECK(optFirst->y_min + 1 == optChinese->y_min);
    CHECK(optChinese->y_min == optEmoji->y_min);
    CHECK(optEmoji->y_min < optTail->y_min);
    auto const chineseOffset = rendered.text.find("文字");
    REQUIRE(chineseOffset != std::string::npos);
    CHECK(rendered.text.find("文字", chineseOffset + std::string_view{"文字"}.size()) == std::string::npos);
    CHECK(track.row.title == title);
  }
} // namespace ao::tui::test
