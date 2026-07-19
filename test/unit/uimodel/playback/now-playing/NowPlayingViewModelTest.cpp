// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>
#include <ao/query/Parser.h>
#include <ao/query/Serializer.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  namespace
  {
    PlaybackService::PlaybackRequest playbackRequest(TrackId trackId, std::string title, std::string artist = {})
    {
      auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
      return PlaybackService::PlaybackRequest{
        .item = NowPlayingInfo{.trackId = trackId, .title = std::move(title), .artist = std::move(artist)},
        .input = audio::PlaybackInput{.filePath = fixturePath, .duration = std::chrono::seconds{1}},
      };
    }
  } // namespace

  TEST_CASE("NowPlayingViewModel - view state generation", "[uimodel][unit][playback]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto notificationService = NotificationService{runtime};
    auto playback = makePlaybackService(executor, libraryFixture.library(), notificationService);
    addReadyAudioProvider(playback);

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto const viewModel = NowPlayingViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial render when idle")
    {
      REQUIRE(!log.empty());
      CHECK(log.last().isActive == false);
      CHECK(log.last().title == "Not Playing");
      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Title) == "Not Playing");
    }

    SECTION("Metadata formatting")
    {
      auto desc = playbackRequest(TrackId{1}, "Song", "Artist");

      REQUIRE(playback.play(desc, ListId{1}));
      REQUIRE(!log.empty());
      CHECK(log.last().title == "Song");
      CHECK(log.last().artist == "Artist");
      CHECK(log.last().combinedStatus == "Artist - Song");

      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Title) == "Song");
      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Artist) == "Artist");
    }

    SECTION("Metadata with empty artist shows Unknown Artist")
    {
      auto desc = playbackRequest(TrackId{1}, "Instrumental");
      REQUIRE(playback.play(desc, ListId{1}));
      CHECK(log.last().title == "Instrumental");
      CHECK(log.last().artist == "Unknown Artist");
      CHECK(log.last().combinedStatus == "Instrumental");
    }

    SECTION("fieldText returns empty for unrelated field")
    {
      auto desc = playbackRequest(TrackId{1}, "Song");
      REQUIRE(playback.play(desc, ListId{1}));
      CHECK(NowPlayingViewModel::fieldText(log.last(), rt::TrackField::Year).empty());
    }

    SECTION("Reveal action")
    {
      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::Reveal, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Reveal);
    }

    SECTION("PlayPause action resolves to resume when not playing")
    {
      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::PlayPause, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Resume);
    }

    SECTION("PlayPause command policy maps transport to pause or resume")
    {
      CHECK(resolveNowPlayingPlayPauseCommand(audio::Transport::Playing) == NowPlayingActionCommand::Type::Pause);
      CHECK(resolveNowPlayingPlayPauseCommand(audio::Transport::Paused) == NowPlayingActionCommand::Type::Resume);
      CHECK(resolveNowPlayingPlayPauseCommand(audio::Transport::Idle) == NowPlayingActionCommand::Type::Resume);
    }

    SECTION("FilterByField with Title")
    {
      auto desc = playbackRequest(TrackId{1}, "Song");
      REQUIRE(playback.play(desc, ListId{1}));

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Navigate);
      CHECK(cmd.navigateQuery == "$title = \"Song\"");
    }

    SECTION("FilterByField with empty artist produces no action")
    {
      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Artist);
      CHECK(cmd.type == NowPlayingActionCommand::Type::None);
    }

    SECTION("FilterByField preserves titles containing both quote types")
    {
      auto desc = playbackRequest(TrackId{1}, "A \"Song\" 'Name'");
      REQUIRE(playback.play(desc, ListId{1}));

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Title);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Navigate);
      CHECK(cmd.navigateQuery == R"($title = "A \"Song\" 'Name'")");

      auto parsed = ao::test::requireValue(query::parse(cmd.navigateQuery));
      CHECK(query::serialize(parsed) == R"($title = "A \"Song\" 'Name'")");
    }

    SECTION("Action resolution")
    {
      auto desc = playbackRequest(TrackId{1}, "Song", "Artist");
      REQUIRE(playback.play(desc, ListId{1}));

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Artist);
      CHECK(cmd.type == NowPlayingActionCommand::Type::Navigate);
      CHECK(cmd.navigateQuery == "$artist = \"Artist\"");
    }

    SECTION("Quoting and escaping in actions")
    {
      auto desc = playbackRequest(TrackId{1}, "A \"Song\"");
      REQUIRE(playback.play(desc, ListId{1}));

      auto const cmd = viewModel.resolveAction(NowPlayingFieldAction::FilterByField, rt::TrackField::Title);
      CHECK(cmd.navigateQuery == R"($title = "A \"Song\"")");
    }
  }

  TEST_CASE("NowPlayingViewModel - reports connecting stream info when audio engine is not ready",
            "[uimodel][unit][playback]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto notificationService = NotificationService{runtime};
    auto playback = makePlaybackService(executor, libraryFixture.library(), notificationService);

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto const viewModel = NowPlayingViewModel{playback, [&log](auto const& view) { log.render(view); }};

    REQUIRE(!log.empty());
    CHECK(log.last().streamInfo == "Connecting to audio engine...");
  }

  TEST_CASE("NowPlayingViewModel - presents an unnamed system-default output through the catalog",
            "[uimodel][unit][playback]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto notificationService = NotificationService{runtime};
    auto playback = makePlaybackService(executor, libraryFixture.library(), notificationService);
    addReadyAudioProvider(
      playback,
      audio::BackendProvider::Status{
        .descriptor = {.id = audio::kBackendPipeWire, .supportedProfiles = {{.id = audio::kProfileShared}}},
        .devices = {{.id = audio::DeviceId{}, .isDefault = true, .backendId = audio::kBackendPipeWire}},
      });

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto const viewModel = NowPlayingViewModel{playback, [&log](auto const& view) { log.render(view); }};

    REQUIRE(playback.play(playbackRequest(TrackId{1}, "Song"), ListId{1}));
    REQUIRE(!log.empty());
    CHECK(log.last().audioPipeline.deviceName == "System Default");
    CHECK(log.last().audioPipeline.deviceIconKind == AudioIconKind::AudioServer);
  }

  TEST_CASE("NowPlayingViewModel - refreshes from playback events until destroyed", "[uimodel][unit][playback]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto executor = InlineExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto notificationService = NotificationService{runtime};
    auto playback = makePlaybackService(executor, libraryFixture.library(), notificationService);
    addReadyAudioProvider(playback);

    auto log = ao::test::RenderLog<NowPlayingViewState>{};
    auto viewModelPtr = std::make_unique<NowPlayingViewModel>(playback, [&log](auto const& view) { log.render(view); });

    REQUIRE(!log.empty());
    log.clear();

    auto const trackId =
      libraryFixture.addTrack({.title = "Event Song", .artist = "Event Artist", .album = "Event Album"});
    REQUIRE(playback.play(playbackRequest(trackId, "Event Song", "Event Artist"), ListId{1}));

    REQUIRE(!log.empty());
    CHECK(log.last().title == "Event Song");
    CHECK(log.last().combinedStatus == "Event Artist - Event Song");

    log.clear();
    viewModelPtr.reset();

    REQUIRE(playback.play(playbackRequest(trackId, "After Destroy", "Event Artist"), ListId{1}));
    CHECK(log.empty());
  }
} // namespace ao::uimodel::test
