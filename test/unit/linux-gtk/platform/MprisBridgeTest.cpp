// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/MprisBridge.h"

#include "platform/MprisArtUrlCache.h"
#include "platform/MprisPlaybackEndpoint.h"
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
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
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
    CHECK(MprisBridge::playbackStatus(audio::Transport::Seeking) == "Playing");
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
    constexpr auto kUnknownBytes = std::array{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};

    CHECK(MprisArtUrlCache::extensionForBytes(kUnknownBytes) == ".img");

    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const resourceId = addResource(runtime.musicLibrary(), kPngBytes);
    auto const trackId = library::test::addTrack(
      runtime.musicLibrary(), library::test::TrackSpec{.title = "Cover Track", .coverArtId = resourceId});
    auto const cacheDir = fixture.tempDir().path() / "mpris-art";
    std::filesystem::create_directories(cacheDir);
    auto const stalePath = cacheDir / (std::to_string(resourceId.raw()) + ".img");

    {
      auto output = std::ofstream{stalePath, std::ios::binary | std::ios::trunc};
      REQUIRE(output);
      output.put('\0');
    }

    auto cache = MprisArtUrlCache{runtime.library(), cacheDir};

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
    CHECK_FALSE(std::filesystem::exists(stalePath));
    CHECK(fileBytesEqual(exportedPath, kPngBytes));
    auto cachedWriteTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours{24};
    std::filesystem::last_write_time(exportedPath, cachedWriteTime);
    cachedWriteTime = std::filesystem::last_write_time(exportedPath);
    CHECK(cache.urlForResource(resourceId) == url);
    CHECK(std::filesystem::last_write_time(exportedPath) == cachedWriteTime);

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

  TEST_CASE("MprisBridge - player methods execute playback commands", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(playback);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack =
      library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = "First", .uri = fixturePath});
    auto const secondTrack =
      library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = "Second", .uri = fixturePath});
    auto queue = uimodel::PlaybackQueueModel{playback, runtime.notifications()};
    std::int32_t playSelectionCount = 0;
    auto commands = uimodel::PlaybackCommandSurface{playback, &queue, [&playSelectionCount] { ++playSelectionCount; }};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchPlayerMethod("PlayPause"));
    CHECK(endpoint.dispatchPlayerMethod("Play"));
    CHECK(playSelectionCount == 2);

    REQUIRE(queue.playQueue({firstTrack, secondTrack}, firstTrack, ListId{8}));
    CHECK(endpoint.dispatchPlayerMethod("Next"));
    CHECK(queue.nowPlayingTrackId() == secondTrack);
    CHECK(endpoint.dispatchPlayerMethod("Previous"));
    CHECK(queue.nowPlayingTrackId() == firstTrack);

    CHECK(endpoint.dispatchPlayerMethod("Pause"));
    CHECK(playback.state().transport == audio::Transport::Paused);
    CHECK(endpoint.dispatchPlayerMethod("Stop"));
    CHECK(playback.state().transport == audio::Transport::Idle);
    CHECK_FALSE(endpoint.dispatchPlayerMethod("Seek"));
  }

  TEST_CASE("MprisBridge - root methods dispatch to injected GTK lifecycle callbacks", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto queue = uimodel::PlaybackQueueModel{fixture.runtime().playback(), fixture.runtime().notifications()};
    auto commands = uimodel::PlaybackCommandSurface{fixture.runtime().playback(), &queue, [] {}};
    std::int32_t raiseCount = 0;
    std::int32_t quitCount = 0;
    auto callbacks = MprisBridge::Callbacks{
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
    };
    auto endpoint = MprisPlaybackEndpoint{fixture.runtime().playback(), commands, callbacks};

    CHECK(endpoint.dispatchRootMethod("Raise"));
    CHECK(endpoint.dispatchRootMethod("Quit"));
    CHECK_FALSE(endpoint.dispatchRootMethod("Unsupported"));
    CHECK(raiseCount == 1);
    CHECK(quitCount == 1);
  }

  TEST_CASE("MprisBridge - unsupported player methods are rejected", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto queue = uimodel::PlaybackQueueModel{fixture.runtime().playback(), fixture.runtime().notifications()};
    auto commands = uimodel::PlaybackCommandSurface{fixture.runtime().playback(), &queue, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{fixture.runtime().playback(), commands, callbacks};

    CHECK_FALSE(endpoint.dispatchPlayerMethod("Seek"));
  }

  TEST_CASE("MprisBridge - capability properties mirror playback command capability", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(playback);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack =
      library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = "First", .uri = fixturePath});
    auto const secondTrack =
      library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = "Second", .uri = fixturePath});
    auto queue = uimodel::PlaybackQueueModel{playback, runtime.notifications()};
    auto commands = uimodel::PlaybackCommandSurface{playback, &queue, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    auto const checkCapability = [&endpoint, &commands](
                                   std::string_view const propertyName, uimodel::PlaybackCommand const command)
    {
      auto const optCapability = endpoint.playerCapabilityProperty(propertyName);
      REQUIRE(optCapability);
      CHECK(*optCapability == commands.capable(command));
    };

    REQUIRE(queue.playQueue({firstTrack, secondTrack}, firstTrack, ListId{8}));

    checkCapability("CanPlay", uimodel::PlaybackCommand::Play);
    checkCapability("CanPause", uimodel::PlaybackCommand::Pause);
    checkCapability("CanGoNext", uimodel::PlaybackCommand::Next);
    checkCapability("CanGoPrevious", uimodel::PlaybackCommand::Previous);

    REQUIRE(endpoint.dispatchPlayerMethod("Next"));
    CHECK(queue.nowPlayingTrackId() == secondTrack);
    REQUIRE(endpoint.dispatchPlayerMethod("Next"));
    CHECK(queue.nowPlayingTrackId() == secondTrack);
    CHECK(playback.state().trackId == secondTrack);

    checkCapability("CanGoNext", uimodel::PlaybackCommand::Next);
    checkCapability("CanGoPrevious", uimodel::PlaybackCommand::Previous);
    CHECK(endpoint.playerCapabilityProperty("CanControl").value_or(false));
    CHECK_FALSE(endpoint.playerCapabilityProperty("Volume").has_value());
  }

  TEST_CASE("MprisBridge - volume setter delegates to playback service normalization", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto queue = uimodel::PlaybackQueueModel{playback, fixture.runtime().notifications()};
    auto commands = uimodel::PlaybackCommandSurface{playback, &queue, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchSetVolume(0.42));
    CHECK(playback.state().volume == Catch::Approx{0.42F});

    CHECK(endpoint.dispatchSetVolume(5.0));
    CHECK(playback.state().volume == 1.0F);

    CHECK(endpoint.dispatchSetVolume(-1.0));
    CHECK(playback.state().volume == 0.0F);

    CHECK(endpoint.dispatchSetRate(1.0));
    CHECK_FALSE(endpoint.dispatchSetRate(2.0));
  }

  TEST_CASE("MprisBridge - shuffle and loop status setters delegate to playback service", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto queue = uimodel::PlaybackQueueModel{playback, fixture.runtime().notifications()};
    auto commands = uimodel::PlaybackCommandSurface{playback, &queue, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchSetShuffle(true));
    CHECK(playback.state().shuffleMode == rt::ShuffleMode::On);

    CHECK(endpoint.dispatchSetLoopStatus("Track"));
    CHECK(playback.state().repeatMode == rt::RepeatMode::One);

    CHECK(endpoint.dispatchSetLoopStatus("Playlist"));
    CHECK(playback.state().repeatMode == rt::RepeatMode::All);

    CHECK(endpoint.dispatchSetLoopStatus("None"));
    CHECK(playback.state().repeatMode == rt::RepeatMode::Off);

    CHECK_FALSE(endpoint.dispatchSetLoopStatus("Album"));
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

    auto commands = uimodel::PlaybackCommandSurface{playback, &queueModel, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchSetLoopStatus("Playlist"));
    CHECK(queueModel.hasNext());
    CHECK(queueModel.peekNext() == track1);

    CHECK(endpoint.dispatchSetLoopStatus("None"));
    REQUIRE_FALSE(queueModel.hasNext());
    REQUIRE_FALSE(queueModel.peekNext());

    CHECK(endpoint.dispatchSetShuffle(true));
    CHECK(queueModel.hasNext());
    CHECK(queueModel.peekNext() == track1);
  }

  TEST_CASE("MprisBridge - seek methods update playback service position", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(playback);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = library::test::addTrack(runtime.musicLibrary(),
                                                 library::test::TrackSpec{.title = "MPRIS Seek",
                                                                          .artist = "Desktop Artist",
                                                                          .album = "Desktop Album",
                                                                          .uri = fixturePath,
                                                                          .duration = std::chrono::seconds{10}});
    auto const nextTrackId = library::test::addTrack(
      runtime.musicLibrary(), library::test::TrackSpec{.title = "MPRIS Next", .uri = fixturePath});

    REQUIRE(playback.restoreSession(rt::PlaybackSessionState{
      .sourceListId = ListId{5},
      .trackId = trackId,
      .positionMs = 5'000,
    }));

    auto queue = uimodel::PlaybackQueueModel{playback, runtime.notifications()};
    auto commands = uimodel::PlaybackCommandSurface{playback, &queue, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchSeek(2'000'000));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{7000});

    CHECK(endpoint.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), 1'500'000));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{1500});

    CHECK(endpoint.dispatchSetPosition("/org/mpris/MediaPlayer2/Track/999", 4'000'000));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{1500});

    CHECK(endpoint.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), -1));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{1500});

    CHECK(endpoint.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), 99'000'000));
    CHECK(playback.state().elapsed == std::chrono::milliseconds{1500});

    REQUIRE(queue.playQueue({trackId, nextTrackId}, trackId, ListId{5}));
    CHECK(endpoint.dispatchSeek(99'000'000));
    CHECK(queue.nowPlayingTrackId() == nextTrackId);
    CHECK(playback.state().trackId == nextTrackId);

    playback.stop();
    CHECK(endpoint.dispatchSeek(1'000'000));
    CHECK(endpoint.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), 1'000'000));
  }
} // namespace ao::gtk::platform::test
