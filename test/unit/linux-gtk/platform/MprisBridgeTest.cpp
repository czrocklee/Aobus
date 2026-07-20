// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/MprisBridge.h"

#include "common/UStringConvert.h"
#include "platform/MprisArtUrlCache.h"
#include "platform/MprisPlaybackEndpoint.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

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
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::gtk::platform::test
{
  namespace
  {
    ResourceId addResource(library::MusicLibrary& library, std::span<std::byte const> bytes)
    {
      auto transaction = library::test::writeTransaction(library);
      auto writer = library.resources().writer(transaction);
      auto resourceId = writer.create(bytes);
      REQUIRE(resourceId);
      REQUIRE(transaction.commit());
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

      for (std::size_t index = 0; index < actual.size(); ++index)
      {
        if (std::byte{static_cast<unsigned char>(actual[index])} != expected[index])
        {
          return false;
        }
      }

      return true;
    }

    rt::ViewId prepareAllTracksView(rt::AppRuntime& runtime)
    {
      runtime.reloadAllTracks();
      auto const* const listOrder = rt::builtinTrackPresentationPreset(rt::kListOrderTrackPresentationId);
      REQUIRE(listOrder != nullptr);
      auto const result =
        runtime.workspace().navigateTo(rt::GlobalViewKind::AllTracks, {.optPresentation = listOrder->spec});
      REQUIRE(result);
      return result->activeViewId;
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
    auto const state = rt::PlaybackTransportSnapshot{
      .duration = std::chrono::seconds{125},
      .nowPlaying =
        rt::NowPlayingInfo{
          .trackId = TrackId{42},
          .coverArtId = ResourceId{77},
          .title = "Keyboard Partita",
          .artist = "Johann Sebastian Bach",
          .album = "Partitas",
        },
    };

    auto const metadata = MprisBridge::metadataForState(state, "file:///tmp/aobus-cover.png");

    CHECK(metadata.trackObjectPath == "/org/mpris/MediaPlayer2/Track/42");
    CHECK(metadata.title == "Keyboard Partita");
    CHECK(metadata.artist == "Johann Sebastian Bach");
    CHECK(metadata.album == "Partitas");
    CHECK(metadata.artUrl == "file:///tmp/aobus-cover.png");
    CHECK(metadata.lengthUs == 125'000'000);
    CHECK(MprisBridge::metadataForState(rt::PlaybackTransportSnapshot{}).trackObjectPath.empty());
  }

  TEST_CASE("toUString - UTF-8 conversion preserves multibyte metadata", "[gtk][regression][mpris]")
  {
    constexpr auto kTitle = std::string_view{"龙卷风"};

    auto const converted = toUString(kTitle);

    CHECK(converted.raw() == kTitle);
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
    auto resourceId = kInvalidResourceId;
    auto trackId = kInvalidTrackId;
    auto fixture = ao::gtk::test::GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      {
        auto const fixtureUri =
          audio::test::installAudioFixture(musicLibrary.rootPath(), "basic_metadata.flac", "cover-track.flac");
        resourceId = addResource(musicLibrary, kPngBytes);
        trackId = library::test::addTrack(
          musicLibrary, library::test::TrackSpec{.title = "Cover Track", .uri = fixtureUri, .coverArtId = resourceId});
      }};
    auto& runtime = fixture.runtime();
    rt::test::addReadyAudioProvider(runtime);
    auto& playback = runtime.playback();
    auto const cacheDir = fixture.tempDir().path() / "mpris-art";
    std::filesystem::create_directories(cacheDir);
    auto const stalePath = cacheDir / (std::to_string(resourceId.raw()) + ".img");

    {
      auto output = std::ofstream{stalePath, std::ios::binary | std::ios::trunc};
      REQUIRE(output);
      output.put('\0');
    }

    auto cache = MprisArtUrlCache{runtime.library(), cacheDir};

    auto const viewId = prepareAllTracksView(runtime);
    REQUIRE(playback.commands().startFromView(viewId, trackId));
    CHECK(playback.snapshot().transport.nowPlaying.coverArtId == resourceId);

    auto const url = cache.urlForResource(playback.snapshot().transport.nowPlaying.coverArtId);
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

    auto const metadata = MprisBridge::metadataForState(playback.snapshot().transport, url);
    CHECK(metadata.artUrl == url);
    CHECK(cache.urlForResource(kInvalidResourceId).empty());
    CHECK(cache.urlForResource(ResourceId{999999}).empty());
  }

  TEST_CASE("MprisBridge - elapsed helpers convert and clamp MPRIS time", "[gtk][unit][mpris]")
  {
    auto state = rt::PlaybackTransportSnapshot{.elapsed = std::chrono::milliseconds{5'000},
                                               .duration = std::chrono::milliseconds{10'000},
                                               .nowPlaying = rt::NowPlayingInfo{.trackId = TrackId{7}}};

    CHECK(MprisBridge::microsecondsFromMilliseconds(std::chrono::milliseconds{1234}) == 1'234'000);
    CHECK(MprisBridge::fromMprisMicroseconds(1'234'567) == std::chrono::milliseconds{1234});
    CHECK(MprisBridge::seekTargetElapsed(state, 2'000'000) == std::chrono::milliseconds{7000});
    CHECK(MprisBridge::seekTargetElapsed(state, -8'000'000) == std::chrono::milliseconds{0});
    CHECK(MprisBridge::seekTargetElapsed(state, 99'000'000) == std::chrono::milliseconds{10'000});
    CHECK(MprisBridge::clampElapsed(state, std::chrono::milliseconds{-1}) == std::chrono::milliseconds{0});
    CHECK(MprisBridge::clampElapsed(state, std::chrono::milliseconds{15'000}) == std::chrono::milliseconds{10'000});
  }

  TEST_CASE("MprisBridge - Seeked follows final seek identity rather than elapsed drift", "[gtk][regression][mpris]")
  {
    auto before = rt::PlaybackTransportSnapshot{.elapsed = std::chrono::milliseconds{100}};
    auto after = before;
    after.elapsed = std::chrono::milliseconds{900};

    CHECK_FALSE(MprisBridge::shouldEmitSeeked(before, after));

    after.positionRevision = rt::PlaybackPositionRevision{.value = 1};
    CHECK_FALSE(MprisBridge::shouldEmitSeeked(before, after));

    after.finalSeekRevision = rt::PlaybackFinalSeekRevision{.value = 1};
    CHECK(MprisBridge::shouldEmitSeeked(before, after));
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
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "First", .uri = fixturePath});
    auto const secondTrack =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Second", .uri = fixturePath});
    auto const viewId = prepareAllTracksView(runtime);
    std::int32_t playSelectionCount = 0;
    auto commands = uimodel::PlaybackCommandSurface{playback, [&playSelectionCount] { ++playSelectionCount; }};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchPlayerMethod("PlayPause"));
    CHECK(endpoint.dispatchPlayerMethod("Play"));
    CHECK(playSelectionCount == 2);

    REQUIRE(playback.commands().startFromView(viewId, firstTrack));
    CHECK(endpoint.dispatchPlayerMethod("Next"));
    CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
    CHECK(endpoint.dispatchPlayerMethod("Previous"));
    CHECK(playback.snapshot().succession.currentTrackId == firstTrack);

    CHECK(endpoint.dispatchPlayerMethod("Pause"));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);
    CHECK(endpoint.dispatchPlayerMethod("Stop"));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
    CHECK_FALSE(endpoint.dispatchPlayerMethod("Seek"));
  }

  TEST_CASE("MprisBridge - root methods dispatch to injected GTK lifecycle callbacks", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto commands = uimodel::PlaybackCommandSurface{playback, [] {}};
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
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchRootMethod("Raise"));
    CHECK(endpoint.dispatchRootMethod("Quit"));
    CHECK_FALSE(endpoint.dispatchRootMethod("Unsupported"));
    CHECK(raiseCount == 1);
    CHECK(quitCount == 1);
  }

  TEST_CASE("MprisBridge - unsupported player methods are rejected", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto commands = uimodel::PlaybackCommandSurface{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK_FALSE(endpoint.dispatchPlayerMethod("Seek"));
  }

  TEST_CASE("MprisBridge - capability properties mirror playback command capability", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const firstTrack =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "First", .uri = fixturePath});
    auto const secondTrack =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Second", .uri = fixturePath});
    auto const viewId = prepareAllTracksView(runtime);
    auto commands = uimodel::PlaybackCommandSurface{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    auto const checkCapability = [&endpoint, &commands](
                                   std::string_view const propertyName, uimodel::PlaybackCommand const command)
    {
      auto const optCapability = endpoint.playerCapabilityProperty(propertyName);
      REQUIRE(optCapability);
      CHECK(*optCapability == commands.isCapable(command));
    };

    REQUIRE(playback.commands().startFromView(viewId, firstTrack));

    checkCapability("CanPlay", uimodel::PlaybackCommand::Play);
    checkCapability("CanPause", uimodel::PlaybackCommand::Pause);
    checkCapability("CanGoNext", uimodel::PlaybackCommand::Next);
    checkCapability("CanGoPrevious", uimodel::PlaybackCommand::Previous);

    REQUIRE(endpoint.dispatchPlayerMethod("Next"));
    CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
    REQUIRE(endpoint.dispatchPlayerMethod("Next"));
    CHECK(playback.snapshot().succession.currentTrackId == secondTrack);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == secondTrack);

    checkCapability("CanGoNext", uimodel::PlaybackCommand::Next);
    checkCapability("CanGoPrevious", uimodel::PlaybackCommand::Previous);
    CHECK(endpoint.playerCapabilityProperty("CanControl").value_or(false));
    CHECK_FALSE(endpoint.playerCapabilityProperty("Volume").has_value());
  }

  TEST_CASE("MprisBridge - volume setter delegates to playback service normalization", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto commands = uimodel::PlaybackCommandSurface{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchSetVolume(0.42));
    CHECK(playback.snapshot().transport.volume.level == Catch::Approx{0.42F});

    CHECK(endpoint.dispatchSetVolume(5.0));
    CHECK(playback.snapshot().transport.volume.level == 1.0F);

    CHECK(endpoint.dispatchSetVolume(-1.0));
    CHECK(playback.snapshot().transport.volume.level == 0.0F);
  }

  TEST_CASE("MprisBridge - rate setter keeps fixed rate and pauses on zero", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Rate Track", .uri = fixturePath});
    auto const viewId = prepareAllTracksView(runtime);
    auto commands = uimodel::PlaybackCommandSurface{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    REQUIRE(playback.commands().startFromView(viewId, trackId));
    REQUIRE(playback.snapshot().transport.transport == audio::Transport::Playing);

    CHECK(endpoint.dispatchSetRate(2.0));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);

    CHECK(endpoint.dispatchSetRate(-1.0));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);

    CHECK(endpoint.dispatchSetRate(0.0));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);

    CHECK(endpoint.dispatchSetRate(1.0));
    CHECK(playback.snapshot().transport.transport == audio::Transport::Paused);

    CHECK_FALSE(endpoint.dispatchSetRate(std::numeric_limits<double>::infinity()));
    CHECK_FALSE(endpoint.dispatchSetRate(std::numeric_limits<double>::quiet_NaN()));
  }

  TEST_CASE("MprisBridge - shuffle and loop status setters delegate to playback sequence", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto commands = uimodel::PlaybackCommandSurface{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchSetShuffle(true));
    CHECK(playback.snapshot().succession.shuffle == rt::ShuffleMode::On);

    CHECK(endpoint.dispatchSetLoopStatus("Track"));
    CHECK(playback.snapshot().succession.repeat == rt::RepeatMode::One);

    CHECK(endpoint.dispatchSetLoopStatus("Playlist"));
    CHECK(playback.snapshot().succession.repeat == rt::RepeatMode::All);

    CHECK(endpoint.dispatchSetLoopStatus("None"));
    CHECK(playback.snapshot().succession.repeat == rt::RepeatMode::Off);

    CHECK_FALSE(endpoint.dispatchSetLoopStatus("Album"));
    CHECK(playback.snapshot().succession.repeat == rt::RepeatMode::Off);
  }

  TEST_CASE("MprisBridge - shuffle and loop status setters update an active sequence", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);

    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    [[maybe_unused]] auto const track1 =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Queue 1", .uri = fixturePath});
    auto const track2 =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Queue 2", .uri = fixturePath});

    auto const viewId = prepareAllTracksView(runtime);
    REQUIRE(playback.commands().startFromView(viewId, track2));
    REQUIRE_FALSE(playback.snapshot().succession.hasNext);

    auto commands = uimodel::PlaybackCommandSurface{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchSetLoopStatus("Playlist"));
    CHECK(playback.snapshot().succession.hasNext);

    CHECK(endpoint.dispatchSetLoopStatus("None"));
    REQUIRE_FALSE(playback.snapshot().succession.hasNext);

    CHECK(endpoint.dispatchSetShuffle(true));
    CHECK(playback.snapshot().succession.hasNext);
  }

  TEST_CASE("MprisBridge - seek methods update playback service position", "[gtk][unit][mpris]")
  {
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& playback = runtime.playback();
    rt::test::addReadyAudioProvider(runtime);
    auto const fixturePath = audio::test::requireAudioFixture("basic_metadata.flac").string();
    auto const trackId = ao::gtk::test::addRuntimeTrack(runtime,
                                                        library::test::TrackSpec{.title = "MPRIS Seek",
                                                                                 .artist = "Desktop Artist",
                                                                                 .album = "Desktop Album",
                                                                                 .uri = fixturePath,
                                                                                 .duration = std::chrono::seconds{10}});
    auto const nextTrackId =
      ao::gtk::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = "MPRIS Next", .uri = fixturePath});

    auto const viewId = prepareAllTracksView(runtime);
    REQUIRE(playback.commands().startFromView(viewId, trackId));
    playback.commands().seek(std::chrono::milliseconds{500});

    auto commands = uimodel::PlaybackCommandSurface{playback, [] {}};
    auto callbacks = MprisBridge::Callbacks{};
    auto endpoint = MprisPlaybackEndpoint{playback, commands, callbacks};

    CHECK(endpoint.dispatchSeek(200'000));
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{700});

    CHECK(endpoint.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), 150'000));
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{150});

    CHECK(endpoint.dispatchSetPosition("/org/mpris/MediaPlayer2/Track/999", 4'000'000));
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{150});

    CHECK(endpoint.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), -1));
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{150});

    CHECK(endpoint.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), 99'000'000));
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{150});

    REQUIRE(playback.commands().startFromView(viewId, trackId));
    CHECK(endpoint.dispatchSeek(99'000'000));
    CHECK(playback.snapshot().succession.currentTrackId == nextTrackId);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == nextTrackId);

    playback.commands().stop();
    CHECK(endpoint.dispatchSeek(1'000'000));
    CHECK(endpoint.dispatchSetPosition(MprisBridge::trackObjectPath(trackId), 1'000'000));
  }
} // namespace ao::gtk::platform::test
