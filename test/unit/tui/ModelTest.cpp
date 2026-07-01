// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Model.h"

#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Transport.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("Model - duration formatting is compact and stable", "[tui][unit][model]")
  {
    CHECK(formatDuration(std::chrono::milliseconds{0}) == "0:00");
    CHECK(formatDuration(std::chrono::seconds{65}) == "1:05");
    CHECK(formatDuration(std::chrono::hours{1} + std::chrono::minutes{2} + std::chrono::seconds{3}) == "1:02:03");
    CHECK(formatDuration(-std::chrono::seconds{5}) == "0:00");
  }

  TEST_CASE("Model - track item chooses useful title and detail text", "[tui][unit][model]")
  {
    auto row = rt::TrackRow{.id = TrackId{7},
                            .optUriPath = std::filesystem::path{"music/untitled.flac"},
                            .title = "Fugue",
                            .artist = "A. Composer",
                            .album = "Keyboard Works",
                            .duration = std::chrono::seconds{125}};

    auto item = makeTrackListItem(row);

    CHECK(item.id == TrackId{7});
    CHECK(item.row.title == "Fugue");
    CHECK(item.detail == "A. Composer - Keyboard Works  2:05");
    CHECK(item.label == "--  Fugue  A. Composer  Keyboard Works");

    row.title.clear();
    item = makeTrackListItem(row);

    CHECK(item.label == "--  untitled.flac  A. Composer  Keyboard Works");

    row.optUriPath.reset();
    row.id = TrackId{99};
    item = makeTrackListItem(row);

    CHECK(item.label == "--  Track 99  A. Composer  Keyboard Works");
  }

  TEST_CASE("Model - library navigation includes all tracks and list hierarchy", "[tui][unit][model]")
  {
    auto lists = std::vector<rt::ListNode>{
      {.id = ListId{2}, .parentId = ListId{1}, .name = "Favorites", .kind = rt::ListNodeKind::Manual},
      {.id = ListId{1}, .name = "Playlists", .kind = rt::ListNodeKind::Folder},
      {.id = ListId{3}, .name = "Live", .kind = rt::ListNodeKind::Smart, .smartExpression = "$title ~ \"Live\""},
    };

    auto items = makeLibraryNavigation(lists);
    auto labels = libraryNavigationLabels(items);

    REQUIRE(items.size() == 4);
    CHECK(items[0].id == rt::kAllTracksListId);
    CHECK(items[0].label == "All Tracks");
    CHECK(items[1].label == "[?] Live");
    CHECK(items[1].detail == "[$title ~ \"Live\"]");
    CHECK(items[2].label == "[+] Playlists");
    CHECK(items[3].label == "  [#] Favorites");
    CHECK(labels[1] == "[?] Live [$title ~ \"Live\"]");
  }

  TEST_CASE("Model - library navigation caps cyclic parent depth", "[tui][regression][model]")
  {
    auto lists = std::vector<rt::ListNode>{
      {.id = ListId{1}, .parentId = ListId{2}, .name = "One", .kind = rt::ListNodeKind::Folder},
      {.id = ListId{2}, .parentId = ListId{1}, .name = "Two", .kind = rt::ListNodeKind::Folder},
    };

    auto items = makeLibraryNavigation(lists);

    REQUIRE(items.size() == 3);
    CHECK((items[1].id == ListId{1} || items[2].id == ListId{1}));
    CHECK((items[1].id == ListId{2} || items[2].id == ListId{2}));
  }

  TEST_CASE("Model - presentation navigation includes builtin and custom views", "[tui][unit][model]")
  {
    auto const custom = std::vector<rt::CustomTrackPresentationPreset>{
      {.label = "Dense Albums", .basePresetId = "albums", .spec = rt::TrackPresentationSpec{.id = "dense-albums"}},
    };

    auto const items = makePresentationNavigation(rt::builtinTrackPresentationPresets(), custom);

    REQUIRE(items.size() > custom.size());
    CHECK(items[0].id == "songs");
    CHECK(items[0].label == "Songs");
    CHECK(items[0].detail == "General-purpose song list.");
    CHECK(items.back().id == "dense-albums");
    CHECK(items.back().label == "Dense Albums");
    CHECK(items.back().detail == "custom from albums");
  }

  TEST_CASE("Model - presentation navigation falls back for sparse preset labels", "[tui][unit][model]")
  {
    auto const builtin = std::vector<rt::TrackPresentationPreset>{
      {.spec = rt::TrackPresentationSpec{.id = "raw"}},
    };
    auto const custom = std::vector<rt::CustomTrackPresentationPreset>{
      {.spec = rt::TrackPresentationSpec{.id = "custom-raw"}},
    };

    auto const items = makePresentationNavigation(builtin, custom);

    REQUIRE(items.size() == 2);
    CHECK(items[0].label == "raw");
    CHECK(items[1].label == "custom-raw");
    CHECK(items[1].detail == "custom");
  }

  TEST_CASE("Model - presentation display labels fall back to default", "[tui][unit][model]")
  {
    CHECK(presentationDisplayId("") == "default");
    CHECK(presentationDisplayId("albums") == "albums");
    CHECK(presentationBadgeLabel("") == "view:default");
    CHECK(presentationBadgeLabel("albums") == "view:albums");
  }

  TEST_CASE("Model - section display names fall back consistently", "[tui][unit][model]")
  {
    CHECK(sectionDisplayName(TrackSection{.primaryText = "Album A"}) == "Album A");
    CHECK(sectionDisplayName(TrackSection{}) == "Untitled Section");
  }

  TEST_CASE("Model - menu labels preserve track order", "[tui][unit][model]")
  {
    auto tracks =
      std::vector<TrackListItem>{{.id = TrackId{1}, .label = "First"}, {.id = TrackId{2}, .label = "Second"}};

    CHECK(menuLabels(tracks) == std::vector<std::string>{"First", "Second"});
  }

  TEST_CASE("Model - track detail lines expose user-facing metadata", "[tui][unit][model]")
  {
    auto row = rt::TrackRow{.id = TrackId{9},
                            .title = "Seven",
                            .artist = "Aimer",
                            .album = "Midnight Sun",
                            .albumArtist = "Various",
                            .genre = "Rock",
                            .composer = "Composer",
                            .duration = std::chrono::seconds{299},
                            .year = 2014,
                            .trackNumber = 7,
                            .trackTotal = 12,
                            .sampleRate = 44100,
                            .bitDepth = 16,
                            .codec = AudioCodec::Flac};

    auto lines = trackDetailLines(row);

    REQUIRE(lines.size() >= 11);
    CHECK(lines[0].label == "Title");
    CHECK(lines[0].value == "Seven");
    CHECK(lines[1].value == "Aimer");
    CHECK(lines[6].value == "2014");
    CHECK(lines[7].value == "7");
    CHECK(lines[8].value == "4:59");
    CHECK(lines[10].value == "44100 Hz");
    CHECK(lines[11].value == "16-bit");
  }

  TEST_CASE("Model - selection clamps to available items", "[tui][unit][model]")
  {
    CHECK(clampSelection(0, 0) == 0);
    CHECK(clampSelection(4, 3) == 2);
    CHECK(clampSelection(1, 3) == 1);
  }

  TEST_CASE("Model - selection summary is one-based and bounded", "[tui][unit][model]")
  {
    CHECK(selectionSummary(0, 0) == "0 tracks");
    CHECK(selectionSummary(12, 0) == "1 / 12 tracks");
    CHECK(selectionSummary(12, 99) == "12 / 12 tracks");
    CHECK(selectionSummary(12, -4) == "1 / 12 tracks");
  }

  TEST_CASE("Model - selection movement is bounded", "[tui][unit][model]")
  {
    CHECK(moveSelection(0, 1, 0) == 0);
    CHECK(moveSelection(4, 1, 5) == 4);
    CHECK(moveSelection(0, -1, 5) == 0);
    CHECK(moveSelection(2, 2, 5) == 4);
    CHECK(moveSelection(2, -2, 5) == 0);
  }

  TEST_CASE("Model - transport labels describe playback state", "[tui][unit][model]")
  {
    CHECK(transportLabel(audio::Transport::Idle) == "Idle");
    CHECK(transportLabel(audio::Transport::Opening) == "Opening");
    CHECK(transportLabel(audio::Transport::Buffering) == "Buffering");
    CHECK(transportLabel(audio::Transport::Playing) == "Playing");
    CHECK(transportLabel(audio::Transport::Paused) == "Paused");
    CHECK(transportLabel(audio::Transport::Seeking) == "Seeking");
    CHECK(transportLabel(audio::Transport::Stopping) == "Stopping");
    CHECK(transportLabel(audio::Transport::Error) == "Error");
  }

  TEST_CASE("Model - active playback transports need clock ticks", "[tui][unit][model]")
  {
    CHECK_FALSE(transportNeedsClockTick(audio::Transport::Idle));
    CHECK(transportNeedsClockTick(audio::Transport::Opening));
    CHECK(transportNeedsClockTick(audio::Transport::Buffering));
    CHECK(transportNeedsClockTick(audio::Transport::Playing));
    CHECK_FALSE(transportNeedsClockTick(audio::Transport::Paused));
    CHECK(transportNeedsClockTick(audio::Transport::Seeking));
    CHECK_FALSE(transportNeedsClockTick(audio::Transport::Stopping));
    CHECK_FALSE(transportNeedsClockTick(audio::Transport::Error));
  }

  TEST_CASE("Model - quality indicator uses GTK quality colors", "[tui][unit][model]")
  {
    auto style = qualityIndicatorStyle(audio::Quality::BitwisePerfect);
    CHECK(style.red == 0xA8);
    CHECK(style.green == 0x55);
    CHECK(style.blue == 0xF7);
    CHECK(style.label == "Bit-perfect output");

    style = qualityIndicatorStyle(audio::Quality::LosslessFloat);
    CHECK(style.red == 0x10);
    CHECK(style.green == 0xB9);
    CHECK(style.blue == 0x81);
    CHECK(style.label == "Lossless Conversion");

    style = qualityIndicatorStyle(audio::Quality::LinearIntervention);
    CHECK(style.red == 0xF5);
    CHECK(style.green == 0x9E);
    CHECK(style.blue == 0x0B);

    style = qualityIndicatorStyle(audio::Quality::LossySource);
    CHECK(style.red == 0x6B);
    CHECK(style.green == 0x72);
    CHECK(style.blue == 0x80);

    style = qualityIndicatorStyle(audio::Quality::Clipped);
    CHECK(style.red == 0xEF);
    CHECK(style.green == 0x44);
    CHECK(style.blue == 0x44);

    style = qualityIndicatorStyle(audio::Quality::Unknown);
    CHECK(style.red == 0x6B);
    CHECK(style.green == 0x72);
    CHECK(style.blue == 0x80);
    CHECK(style.label == "Unknown quality");
  }
} // namespace ao::tui::test
