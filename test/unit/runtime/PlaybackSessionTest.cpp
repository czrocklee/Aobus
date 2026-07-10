// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/PlaybackSessionState.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/PlaybackQueueService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    TrackId addPlayableTrack(AppRuntime& runtime, std::string title)
    {
      return library::test::addTrack(runtime.musicLibrary(),
                                     library::test::TrackSpec{
                                       .title = std::move(title),
                                       .uri = audio::test::requireAudioFixture("basic_metadata.flac").string(),
                                       .duration = std::chrono::seconds{10},
                                     });
    }

    PlaybackSessionState storedSession(ConfigStore& store)
    {
      auto session = PlaybackSessionState{};
      REQUIRE(store.load(kPlaybackSessionConfigGroup, session));
      return session;
    }
  } // namespace

  TEST_CASE("AppRuntime playback session - ordered queue and playback state round-trip coherently",
            "[runtime][unit][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto firstTrack = TrackId{};
    auto secondTrack = TrackId{};
    auto thirdTrack = TrackId{};

    {
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime.playback());
      firstTrack = addPlayableTrack(runtime, "First");
      secondTrack = addPlayableTrack(runtime, "Second");
      thirdTrack = addPlayableTrack(runtime, "Third");
      REQUIRE(runtime.playbackQueue().playQueue({thirdTrack, firstTrack, secondTrack}, firstTrack, kAllTracksListId));
      runtime.playback().seek(std::chrono::milliseconds{500});
      runtime.playback().setShuffleMode(ShuffleMode::On);
      runtime.playback().setRepeatMode(RepeatMode::All);
      runtime.playback().setVolume(0.5F);
      runtime.playback().setMuted(true);
      REQUIRE(runtime.savePlaybackSession());
      CHECK(storedSession(runtime.configStore()).positionMs == 500);
    }

    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto published = std::vector<PlaybackQueueState>{};
    auto sub = runtime.playbackQueue().onChanged([&](PlaybackQueueState const& state) { published.push_back(state); });

    auto const restored = runtime.restorePlaybackSession();

    REQUIRE(restored);
    REQUIRE(restored->restored);
    CHECK(restored->trackId == firstTrack);
    CHECK(restored->sourceListId == kAllTracksListId);
    REQUIRE(published.size() == 1);
    CHECK(published.front().trackIds == std::vector{thirdTrack, firstTrack, secondTrack});
    REQUIRE(published.front().optCurrentIndex);
    CHECK(*published.front().optCurrentIndex == 1);
    auto const& playback = runtime.playback().state();
    CHECK(playback.transport == audio::Transport::Idle);
    CHECK(playback.nowPlaying.trackId == firstTrack);
    CHECK(playback.elapsed == std::chrono::milliseconds{500});
    CHECK(playback.mode.shuffle == ShuffleMode::On);
    CHECK(playback.mode.repeat == RepeatMode::All);
    CHECK(playback.volume.level == 0.5F);
    CHECK(playback.volume.muted);
  }

  TEST_CASE("AppRuntime playback session - deleted source falls back and stale non-current tracks are filtered",
            "[runtime][unit][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const firstTrack = addPlayableTrack(runtime, "First");
    auto const staleTrack = addPlayableTrack(runtime, "Stale");
    auto const currentTrack = addPlayableTrack(runtime, "Current");
    auto const sourceListId = ao::test::requireValue(
      runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Temporary source"}));
    REQUIRE(runtime.playbackQueue().playQueue({firstTrack, staleTrack, currentTrack}, currentTrack, sourceListId));
    REQUIRE(runtime.savePlaybackSession());
    REQUIRE(runtime.library().writer().deleteTrack(staleTrack));
    REQUIRE(runtime.library().writer().deleteList(sourceListId));
    runtime.playback().stop();

    auto const restored = runtime.restorePlaybackSession();

    REQUIRE(restored);
    REQUIRE(restored->restored);
    CHECK(restored->sourceListId == kAllTracksListId);
    CHECK(runtime.playbackQueue().state().trackIds == std::vector{firstTrack, currentTrack});
    REQUIRE(runtime.playbackQueue().state().optCurrentIndex);
    CHECK(*runtime.playbackQueue().state().optCurrentIndex == 1);
  }

  TEST_CASE("AppRuntime playback session - deleted current track fails without partial mutation",
            "[runtime][unit][playback-session][error]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const savedTrack = addPlayableTrack(runtime, "Saved");
    auto const liveTrack = addPlayableTrack(runtime, "Live");
    REQUIRE(runtime.playbackQueue().playQueue({savedTrack}, savedTrack, kAllTracksListId));
    REQUIRE(runtime.savePlaybackSession());
    REQUIRE(runtime.library().writer().deleteTrack(savedTrack));
    REQUIRE(runtime.playbackQueue().playQueue({liveTrack}, liveTrack, kAllTracksListId));
    auto const queueBefore = runtime.playbackQueue().state();
    auto const playbackBefore = runtime.playback().state();

    auto const restored = runtime.restorePlaybackSession();

    REQUIRE_FALSE(restored);
    CHECK(restored.error().code == Error::Code::NotFound);
    CHECK(runtime.playbackQueue().state().trackIds == queueBefore.trackIds);
    CHECK(runtime.playbackQueue().state().optCurrentIndex == queueBefore.optCurrentIndex);
    CHECK(runtime.playback().state().nowPlaying == playbackBefore.nowPlaying);
    CHECK(runtime.playback().state().transport == playbackBefore.transport);
  }

  TEST_CASE("AppRuntime playback session - unsupported and inconsistent payloads do not mutate runtime",
            "[runtime][unit][playback-session][error]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);
    addReadyAudioProvider(runtime.playback());
    auto const savedTrack = addPlayableTrack(runtime, "Saved");
    auto const liveTrack = addPlayableTrack(runtime, "Live");
    REQUIRE(runtime.playbackQueue().playQueue({liveTrack}, liveTrack, kAllTracksListId));
    auto const queueBefore = runtime.playbackQueue().state();

    auto invalid = PlaybackSessionState{
      .schemaVersion = kPlaybackSessionSchemaVersion + 1,
      .queueTrackIds = {savedTrack},
      .currentQueueIndex = 0,
      .sourceListId = kAllTracksListId,
      .trackId = savedTrack,
    };
    REQUIRE(runtime.configStore().saveResult(kPlaybackSessionConfigGroup, invalid));
    auto unsupported = runtime.restorePlaybackSession();
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error().code == Error::Code::FormatRejected);
    CHECK(runtime.playbackQueue().state().trackIds == queueBefore.trackIds);

    invalid.schemaVersion = kPlaybackSessionSchemaVersion;
    invalid.currentQueueIndex = 1;
    REQUIRE(runtime.configStore().saveResult(kPlaybackSessionConfigGroup, invalid));
    auto inconsistent = runtime.restorePlaybackSession();
    REQUIRE_FALSE(inconsistent);
    CHECK(inconsistent.error().code == Error::Code::CorruptData);
    CHECK(runtime.playbackQueue().state().trackIds == queueBefore.trackIds);
    CHECK(runtime.playback().state().nowPlaying.trackId == liveTrack);
  }

  TEST_CASE("AppRuntime playback session - missing config is a successful empty restore",
            "[runtime][unit][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const restored = runtime.restorePlaybackSession();

    REQUIRE(restored);
    CHECK_FALSE(restored->restored);
    CHECK(runtime.playbackQueue().state().trackIds.empty());
    CHECK(runtime.playback().state().nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("AppRuntime playback session - config load and save failures retain Result diagnostics",
            "[runtime][unit][playback-session][error]")
  {
    SECTION("malformed config load")
    {
      auto tempDir = ao::test::TempDir{};
      std::ofstream{tempDir.path() / "workspace.yaml"} << "playback-session: [not, a, map]\n";
      auto runtime = makeRuntime(tempDir);

      auto const restored = runtime.restorePlaybackSession();

      REQUIRE_FALSE(restored);
      CHECK(restored.error().code == Error::Code::FormatRejected);
    }

    SECTION("config save path is not writable as a file")
    {
      auto tempDir = ao::test::TempDir{};
      REQUIRE(std::filesystem::create_directory(tempDir.path() / "workspace.yaml"));
      auto runtime = makeRuntime(tempDir);
      addReadyAudioProvider(runtime.playback());
      auto const trackId = addPlayableTrack(runtime, "Track");
      REQUIRE(runtime.playbackQueue().playQueue({trackId}, trackId, kAllTracksListId));

      auto const saved = runtime.savePlaybackSession();

      REQUIRE_FALSE(saved);
      CHECK(saved.error().code == Error::Code::IoError);
    }
  }

  TEST_CASE("Playback session schema - representative ordered queue has a bounded serialized size",
            "[runtime][unit][playback-session][scale]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "queue-size.yaml";
    auto store = ConfigStore{configPath};
    auto session = PlaybackSessionState{};
    session.queueTrackIds.reserve(100'000);

    for (std::uint32_t value = 1; value <= 100'000; ++value)
    {
      session.queueTrackIds.emplace_back(value);
    }

    session.currentQueueIndex = 50'000;
    session.trackId = session.queueTrackIds[50'000];
    session.sourceListId = kAllTracksListId;
    REQUIRE(store.saveResult(kPlaybackSessionConfigGroup, session));
    REQUIRE(store.flush());

    auto const bytes = std::filesystem::file_size(configPath);
    CHECK(bytes > 100'000);
    CHECK(bytes < 4'000'000);
    auto const reloaded = storedSession(store);
    CHECK(reloaded.queueTrackIds.size() == session.queueTrackIds.size());
    CHECK(reloaded.trackId == session.trackId);
  }
} // namespace ao::rt::test
