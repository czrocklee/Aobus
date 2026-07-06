// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/MprisBridge.h"

#include "platform/MprisArtUrlCache.h"
#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/PlaybackSessionState.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/layout/action/LayoutActionTypes.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <giomm/file.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::platform::test
{
  namespace
  {
    uimodel::LayoutActionActivationOutcome activated()
    {
      return uimodel::LayoutActionActivationOutcome{
        .result = uimodel::LayoutActionActivationResult::Activated, .state = {.enabled = true, .disabledReason = ""}};
    }

    uimodel::LayoutActionActivationOutcome disabled()
    {
      return uimodel::LayoutActionActivationOutcome{
        .result = uimodel::LayoutActionActivationResult::Disabled, .state = {.enabled = false, .disabledReason = ""}};
    }

    ResourceId addResource(library::MusicLibrary& library, std::span<std::byte const> bytes)
    {
      auto txn = library.writeTransaction();
      auto writer = library.resources().writer(txn);
      auto resourceId = writer.create(bytes);
      REQUIRE(resourceId);
      REQUIRE(txn.commit());
      return *resourceId;
    }

    std::filesystem::path pathFromFileUrl(std::string const& url)
    {
      auto const filePtr = Gio::File::create_for_uri(url);
      REQUIRE(filePtr);
      auto path = filePtr->get_path();
      REQUIRE(!path.empty());
      return std::filesystem::path{path};
    }

    bool fileBytesEqual(std::filesystem::path const& path, std::span<std::byte const> expected)
    {
      auto const actual = ao::test::readFile(path);

      if (actual.size() != expected.size())
      {
        return false;
      }

      for (std::size_t idx = 0; idx < actual.size(); ++idx)
      {
        if (std::byte{static_cast<unsigned char>(actual[idx])} != expected[idx])
        {
          return false;
        }
      }

      return true;
    }
  } // namespace

  TEST_CASE("MprisBridge - playback status maps transport to MPRIS states", "[gtk][unit][mpris]")
  {
    CHECK(MprisBridge::playbackStatus(audio::Transport::Opening) == "Playing");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Buffering) == "Playing");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Playing) == "Playing");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Paused) == "Paused");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Idle) == "Stopped");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Seeking) == "Stopped");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Stopping) == "Stopped");
    CHECK(MprisBridge::playbackStatus(audio::Transport::Error) == "Stopped");
  }

  TEST_CASE("MprisBridge - metadata snapshot maps playback state to MPRIS fields", "[gtk][unit][mpris]")
  {
    auto const state = rt::PlaybackState{.trackId = TrackId{42},
                                         .trackCoverArtId = ResourceId{77},
                                         .trackTitle = "Keyboard Partita",
                                         .trackArtist = "Johann Sebastian Bach",
                                         .trackAlbum = "Partitas",
                                         .duration = std::chrono::seconds{125}};

    auto const metadata = MprisBridge::metadataForState(state, "file:///tmp/aobus-cover.png");

    CHECK(metadata.trackObjectPath == "/org/mpris/MediaPlayer2/Track/42");
    CHECK(metadata.title == "Keyboard Partita");
    CHECK(metadata.artist == "Johann Sebastian Bach");
    CHECK(metadata.album == "Partitas");
    CHECK(metadata.artUrl == "file:///tmp/aobus-cover.png");
    CHECK(metadata.lengthUs == 125'000'000);
    CHECK(MprisBridge::metadataForState(rt::PlaybackState{}).trackObjectPath.empty());
  }

  TEST_CASE("MprisArtUrlCache - exports library cover art resources as file URLs", "[gtk][unit][mpris]")
  {
    constexpr auto kPngBytes = std::array{std::byte{0x89},
                                          std::byte{0x50},
                                          std::byte{0x4E},
                                          std::byte{0x47},
                                          std::byte{0x0D},
                                          std::byte{0x0A},
                                          std::byte{0x1A},
                                          std::byte{0x0A},
                                          std::byte{0x00},
                                          std::byte{0x01}};

    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const resourceId = addResource(runtime.musicLibrary(), kPngBytes);
    auto const trackId = library::test::addTrack(
      runtime.musicLibrary(), library::test::TrackSpec{.title = "Cover Track", .coverArtId = resourceId});
    auto cache = MprisArtUrlCache{runtime.library(), fixture.tempDir().path() / "mpris-art"};

    REQUIRE(runtime.playback().restoreSession(rt::PlaybackSessionState{
      .sourceListId = ListId{5},
      .trackId = trackId,
    }));
    CHECK(runtime.playback().state().trackCoverArtId == resourceId);

    auto const url = cache.urlForResource(runtime.playback().state().trackCoverArtId);
    REQUIRE(url.starts_with("file://"));

    auto const exportedPath = pathFromFileUrl(url);
    CHECK(exportedPath.extension() == ".png");
    CHECK(std::filesystem::is_regular_file(exportedPath));
    CHECK(fileBytesEqual(exportedPath, kPngBytes));
    CHECK(cache.urlForResource(resourceId) == url);

    REQUIRE(std::filesystem::remove(exportedPath));
    CHECK(cache.urlForResource(resourceId) == url);
    CHECK(std::filesystem::is_regular_file(exportedPath));
    CHECK(fileBytesEqual(exportedPath, kPngBytes));

    {
      auto output = std::ofstream{exportedPath, std::ios::binary | std::ios::trunc};
      REQUIRE(output);
      output.put('\0');
    }

    CHECK(cache.urlForResource(resourceId) == url);
    CHECK(fileBytesEqual(exportedPath, kPngBytes));

    auto const metadata = MprisBridge::metadataForState(runtime.playback().state(), url);
    CHECK(metadata.artUrl == url);
    CHECK(cache.urlForResource(kInvalidResourceId).empty());
    CHECK(cache.urlForResource(ResourceId{999999}).empty());
  }

  TEST_CASE("MprisBridge - elapsed helpers convert and clamp MPRIS time", "[gtk][unit][mpris]")
  {
    auto state = rt::PlaybackState{.trackId = TrackId{7},
                                   .elapsed = std::chrono::milliseconds{5'000},
                                   .duration = std::chrono::milliseconds{10'000}};

    CHECK(MprisBridge::microsecondsFromMilliseconds(std::chrono::milliseconds{1234}) == 1'234'000);
    CHECK(MprisBridge::fromMprisMicroseconds(1'234'567) == std::chrono::milliseconds{1234});
    CHECK(MprisBridge::seekTargetElapsed(state, 2'000'000) == std::chrono::milliseconds{7000});
    CHECK(MprisBridge::seekTargetElapsed(state, -8'000'000) == std::chrono::milliseconds{0});
    CHECK(MprisBridge::seekTargetElapsed(state, 99'000'000) == std::chrono::milliseconds{10'000});
    CHECK(MprisBridge::clampElapsed(state, std::chrono::milliseconds{-1}) == std::chrono::milliseconds{0});
    CHECK(MprisBridge::clampElapsed(state, std::chrono::milliseconds{15'000}) == std::chrono::milliseconds{10'000});
  }

  TEST_CASE("MprisBridge - loop status maps runtime repeat modes", "[gtk][unit][mpris]")
  {
    CHECK(MprisBridge::loopStatus(rt::RepeatMode::Off) == "None");
    CHECK(MprisBridge::loopStatus(rt::RepeatMode::One) == "Track");
    CHECK(MprisBridge::loopStatus(rt::RepeatMode::All) == "Playlist");

    auto const optOff = MprisBridge::repeatModeForLoopStatus("None");
    REQUIRE(optOff);
    CHECK(*optOff == rt::RepeatMode::Off);

    auto const optOne = MprisBridge::repeatModeForLoopStatus("Track");
    REQUIRE(optOne);
    CHECK(*optOne == rt::RepeatMode::One);

    auto const optAll = MprisBridge::repeatModeForLoopStatus("Playlist");
    REQUIRE(optAll);
    CHECK(*optAll == rt::RepeatMode::All);
    CHECK_FALSE(MprisBridge::repeatModeForLoopStatus("Album").has_value());
  }

  TEST_CASE("MprisBridge - player methods activate stable app actions", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto activatedActions = std::vector<std::string>{};
    auto bridge =
      MprisBridge{fixture.runtime().playback(),
                  MprisBridge::Callbacks{.activateAction = [&activatedActions](std::string_view const actionId)
                                         {
                                           activatedActions.emplace_back(actionId);
                                           return activated();
                                         }}};

    CHECK(bridge.dispatchPlayerMethod("PlayPause"));
    CHECK(bridge.dispatchPlayerMethod("Play"));
    CHECK(bridge.dispatchPlayerMethod("Pause"));
    CHECK(bridge.dispatchPlayerMethod("Stop"));
    CHECK(bridge.dispatchPlayerMethod("Next"));
    CHECK(bridge.dispatchPlayerMethod("Previous"));

    CHECK(activatedActions == std::vector<std::string>{"playback.playPause",
                                                       "playback.play",
                                                       "playback.pause",
                                                       "playback.stop",
                                                       "playback.next",
                                                       "playback.previous"});
  }

  TEST_CASE("MprisBridge - root methods dispatch to injected GTK lifecycle callbacks", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    std::int32_t raiseCount = 0;
    std::int32_t quitCount = 0;
    auto bridge = MprisBridge{fixture.runtime().playback(),
                              MprisBridge::Callbacks{
                                .activateAction = [](std::string_view const) { return disabled(); },
                                .raise =
                                  [&raiseCount]
                                {
                                  ++raiseCount;
                                  return true;
                                },
                                .quit =
                                  [&quitCount]
                                {
                                  ++quitCount;
                                  return true;
                                },
                              }};

    CHECK(bridge.dispatchRootMethod("Raise"));
    CHECK(bridge.dispatchRootMethod("Quit"));
    CHECK_FALSE(bridge.dispatchRootMethod("Unsupported"));
    CHECK(raiseCount == 1);
    CHECK(quitCount == 1);
  }

  TEST_CASE("MprisBridge - disabled player methods are handled as no-ops", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto bridge =
      MprisBridge{fixture.runtime().playback(),
                  MprisBridge::Callbacks{.activateAction = [](std::string_view const) { return disabled(); }}};

    CHECK(bridge.dispatchPlayerMethod("Next"));
    CHECK_FALSE(bridge.dispatchPlayerMethod("Seek"));
  }

  TEST_CASE("MprisBridge - volume setter delegates to playback service normalization", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto bridge = MprisBridge{
      playback, MprisBridge::Callbacks{.activateAction = [](std::string_view const) { return disabled(); }}};

    CHECK(bridge.dispatchSetVolume(0.42));
    CHECK(playback.state().volume == Catch::Approx{0.42F});

    CHECK(bridge.dispatchSetVolume(5.0));
    CHECK(playback.state().volume == 1.0F);

    CHECK(bridge.dispatchSetVolume(-1.0));
    CHECK(playback.state().volume == 0.0F);
  }

  TEST_CASE("MprisBridge - shuffle and loop status setters delegate to playback service", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto bridge = MprisBridge{
      playback, MprisBridge::Callbacks{.activateAction = [](std::string_view const) { return disabled(); }}};

    CHECK(bridge.dispatchSetShuffle(true));
    CHECK(playback.state().shuffleMode == rt::ShuffleMode::On);

    CHECK(bridge.dispatchSetLoopStatus("Track"));
    CHECK(playback.state().repeatMode == rt::RepeatMode::One);

    CHECK(bridge.dispatchSetLoopStatus("Playlist"));
    CHECK(playback.state().repeatMode == rt::RepeatMode::All);

    CHECK(bridge.dispatchSetLoopStatus("None"));
    CHECK(playback.state().repeatMode == rt::RepeatMode::Off);

    CHECK_FALSE(bridge.dispatchSetLoopStatus("Album"));
    CHECK(playback.state().repeatMode == rt::RepeatMode::Off);
  }

  TEST_CASE("MprisBridge - shuffle and loop status setters re-prepare an active queue", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(playback);

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const track1 =
      library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = "Queue 1", .uri = fixturePath});
    auto const track2 =
      library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = "Queue 2", .uri = fixturePath});

    auto queueModel = uimodel::PlaybackQueueModel{playback, runtime.notifications()};
    REQUIRE(queueModel.playQueue({track1, track2}, track2, ListId{10}));
    REQUIRE_FALSE(queueModel.hasNext());
    REQUIRE_FALSE(queueModel.peekNext());

    auto bridge = MprisBridge{
      playback, MprisBridge::Callbacks{.activateAction = [](std::string_view const) { return disabled(); }}};

    CHECK(bridge.dispatchSetLoopStatus("Playlist"));
    CHECK(queueModel.hasNext());
    CHECK(queueModel.peekNext() == track1);

    CHECK(bridge.dispatchSetLoopStatus("None"));
    REQUIRE_FALSE(queueModel.hasNext());
    REQUIRE_FALSE(queueModel.peekNext());

    CHECK(bridge.dispatchSetShuffle(true));
    CHECK(queueModel.hasNext());
    CHECK(queueModel.peekNext() == track1);
  }

  TEST_CASE("MprisBridge - seek methods update playback service position", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    auto const trackId = library::test::addTrack(runtime.musicLibrary(),
                                                 library::test::TrackSpec{.title = "MPRIS Seek",
                                                                          .artist = "Desktop Artist",
                                                                          .album = "Desktop Album",
                                                                          .duration = std::chrono::seconds{10}});

    REQUIRE(playback.restoreSession(rt::PlaybackSessionState{
      .sourceListId = ListId{5},
      .trackId = trackId,
      .positionMs = 5'000,
    }));

    auto activatedActions = std::vector<std::string>{};
    auto bridge =
      MprisBridge{playback,
                  MprisBridge::Callbacks{.activateAction = [&activatedActions](std::string_view const actionId)
                                         {
                                           activatedActions.emplace_back(actionId);
                                           return disabled();
                                         }}};

    CHECK(bridge.dispatchSeek(2'000'000));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{7000});

    CHECK(bridge.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), 1'500'000));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{1500});

    CHECK(bridge.dispatchSetPosition("/org/mpris/MediaPlayer2/Track/999", 4'000'000));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{1500});

    CHECK(bridge.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), -1));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{1500});

    CHECK(bridge.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), 99'000'000));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{1500});

    CHECK(bridge.dispatchSeek(99'000'000));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{1500});
    CHECK(activatedActions == std::vector<std::string>{"playback.next"});

    playback.stop();
    CHECK_FALSE(bridge.dispatchSeek(1'000'000));
    CHECK_FALSE(bridge.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), 1'000'000));
  }
} // namespace ao::gtk::platform::test
