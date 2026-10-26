// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/media/wav/TestWav.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::vector<TrackId> importSessionCarrier(AppRuntime& runtime, std::uint64_t const totalFrames)
    {
      auto const bytes = ao::test::wav::makeWav(ao::test::wav::Spec{
        .sampleRate = 44100,
        .audioData = std::vector<std::uint8_t>(static_cast<std::size_t>(totalFrames) * 2, 0x21),
      });
      auto const file = ao::test::TempFile{bytes, ".wav"};
      std::filesystem::copy_file(file.path, runtime.musicRoot() / "disc.wav");
      {
        auto output = std::ofstream{runtime.musicRoot() / "disc.cue"};
        output << "FILE ignored.wav WAVE\n"
                  "TRACK 01 AUDIO\nTITLE First\nINDEX 01 00:00:00\n"
                  "TRACK 02 AUDIO\nTITLE Second\nINDEX 01 00:00:01\n";
      }

      auto createdRes = runRuntimeTask(
        runtime, runtime.library().commands().createTrackFromFileAsync(runtime.musicRoot() / "disc.wav"));
      REQUIRE(createdRes);
      REQUIRE(createdRes->trackIds.size() == 2);
      runtime.sources().reloadAllTracks();
      return createdRes->trackIds;
    }

    PlaybackSessionState sessionForCarrier(AppRuntime& runtime, TrackId const trackId)
    {
      auto const reader = runtime.library().snapshot();
      auto const optSegment = reader.trackSegment(trackId);
      REQUIRE(optSegment);
      return PlaybackSessionState{
        .libraryId = reader.libraryId(),
        .sourceListId = kAllTracksListId,
        .currentTrackId = trackId,
        .currentTrackUri = "disc.wav",
        .currentTrackOrdinal = optSegment->ordinal,
        .anchorIndex = 1,
      };
    }

    void saveCarrierSession(AppRuntime& runtime, PlaybackSessionState const& session)
    {
      REQUIRE(runtime.workspaceConfigStore().save(kPlaybackSessionConfigGroup, session, PlaybackSessionYamlSchema{}));
    }

    PlaybackSessionState readCarrierSession(AppRuntime& runtime)
    {
      auto state = PlaybackSessionState{};
      auto const loadedRes =
        runtime.workspaceConfigStore().load(kPlaybackSessionConfigGroup, state, PlaybackSessionYamlSchema{});
      REQUIRE(loadedRes);
      REQUIRE(*loadedRes);
      return state;
    }
  } // namespace

  TEST_CASE("CUE playback session - identity includes the immutable ordinal beside the URI")
  {
    auto const temp = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(temp);
    auto const ids = importSessionCarrier(*runtimePtr, 1176);
    auto session = sessionForCarrier(*runtimePtr, ids[1]);
    REQUIRE(session.currentTrackOrdinal == 2);
    session.currentTrackOrdinal = 1;
    saveCarrierSession(*runtimePtr, session);
    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    CHECK_FALSE(restoredRes->restored);
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("CUE playback session - a replacement ID for the same ordinal drops the saved payload")
  {
    auto const temp = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(temp);
    auto const ids = importSessionCarrier(*runtimePtr, 1176);
    auto session = sessionForCarrier(*runtimePtr, ids[1]);
    session.currentTrackId = TrackId{999999};
    saveCarrierSession(*runtimePtr, session);
    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    CHECK_FALSE(restoredRes->restored);
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("CUE playback session - an absent ordinal still permits ordinary anchor replacement")
  {
    auto const temp = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(temp);
    auto const ids = importSessionCarrier(*runtimePtr, 1176);
    auto session = sessionForCarrier(*runtimePtr, ids[1]);
    session.currentTrackId = TrackId{999999};
    session.currentTrackOrdinal = 9;
    session.positionMs = 5;
    saveCarrierSession(*runtimePtr, session);
    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == ids[1]);
    CHECK(runtimePtr->playback().snapshot().transport.elapsed == std::chrono::milliseconds{0});
  }

  TEST_CASE("CUE playback session - a carrier relinked while closed drops the saved session")
  {
    auto const temp = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(temp);
    auto const ids = importSessionCarrier(*runtimePtr, 1176);
    saveCarrierSession(*runtimePtr, sessionForCarrier(*runtimePtr, ids[1]));
    auto& jobs = runtimePtr->library().jobs();
    REQUIRE(runRuntimeTask(*runtimePtr, jobs.backfillAudioIdentityAsync()));
    std::filesystem::rename(runtimePtr->musicRoot() / "disc.wav", runtimePtr->musicRoot() / "moved.wav");
    std::filesystem::rename(runtimePtr->musicRoot() / "disc.cue", runtimePtr->musicRoot() / "moved.cue");
    auto planRes = runRuntimeTask(*runtimePtr, jobs.buildScanPlanAsync());
    REQUIRE(planRes);
    REQUIRE(planRes->count(ScanClassification::Moved) == 1);
    auto const appliedRes = runRuntimeTask(*runtimePtr, jobs.applyScanPlanAsync(std::move(*planRes)));
    REQUIRE(appliedRes);
    REQUIRE(appliedRes->relinkedIds == ids);
    auto const optUri = runtimePtr->library().snapshot().trackUri(ids[1]);
    REQUIRE(optUri);
    REQUIRE(*optUri == "moved.wav");

    // The member keeps its id and ordinal, but a relink made while closed looks
    // like a rollback that reissued the id, so the session drops as it does for
    // a whole file.
    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    CHECK_FALSE(restoredRes->restored);
    CHECK(runtimePtr->playback().snapshot().transport.nowPlaying.trackId == kInvalidTrackId);
  }

  TEST_CASE("CUE playback session - deferred segment endpoint ends without opening media and saves its ordinal")
  {
    auto const temp = ao::test::TempDir{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(temp, std::move(executorPtr));
    addReadyAudioProvider(*runtimePtr);
    executor->runUntilIdle();
    auto const ids = importSessionCarrier(*runtimePtr, 1176);
    saveCarrierSession(*runtimePtr, sessionForCarrier(*runtimePtr, ids[1]));
    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    auto& playback = runtimePtr->playback();
    auto const restored = playback.snapshot().transport;
    REQUIRE(restored.duration == std::chrono::milliseconds{13});
    REQUIRE(playback.commands().trySeek(restored.occurrenceId, restored.duration));
    CHECK(playback.snapshot().transport.elapsed == restored.duration);
    REQUIRE(runtimePtr->savePlaybackSession());
    auto const saved = readCarrierSession(*runtimePtr);
    CHECK(saved.currentTrackUri == "disc.wav");
    CHECK(saved.currentTrackOrdinal == 2);
    CHECK(saved.positionMs == 0);
    REQUIRE(std::filesystem::remove(runtimePtr->musicRoot() / "disc.wav"));
    playback.commands().resume();
    executor->runUntilIdle();
    CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
    CHECK(playback.snapshot().transport.occurrenceId.value == 0);
  }

  TEST_CASE("CUE playback session - restored zero millisecond segment remains playable after seeking zero")
  {
    auto const temp = ao::test::TempDir{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(temp, std::move(executorPtr));
    addReadyAudioProvider(*runtimePtr);
    executor->runUntilIdle();
    auto const ids = importSessionCarrier(*runtimePtr, 589);
    saveCarrierSession(*runtimePtr, sessionForCarrier(*runtimePtr, ids[1]));
    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    auto& playback = runtimePtr->playback();
    CHECK(playback.snapshot().transport.duration == std::chrono::milliseconds{0});
    playback.commands().seek(std::chrono::milliseconds{0});
    playback.commands().resume();
    executor->runUntilIdle();
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == ids[1]);
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK(readCarrierSession(*runtimePtr).currentTrackOrdinal == 2);
  }

  TEST_CASE("CUE playback session - zero millisecond segment normalizes a saved positive offset before resume")
  {
    auto const temp = ao::test::TempDir{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(temp, std::move(executorPtr));
    addReadyAudioProvider(*runtimePtr);
    executor->runUntilIdle();
    // The second member contains one frame at 44.1 kHz, not an unknown duration.
    auto const ids = importSessionCarrier(*runtimePtr, 589);
    auto session = sessionForCarrier(*runtimePtr, ids[1]);
    session.positionMs = 1;
    saveCarrierSession(*runtimePtr, session);

    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    auto& playback = runtimePtr->playback();
    CHECK(playback.snapshot().transport.duration == std::chrono::milliseconds{0});
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{0});
    // Restore normalizes in memory; a later checkpoint persists that value.
    CHECK(readCarrierSession(*runtimePtr).positionMs == 1);
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK(readCarrierSession(*runtimePtr).positionMs == 0);

    playback.commands().resume();
    executor->runUntilIdle();
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == ids[1]);
    REQUIRE(runtimePtr->savePlaybackSession());
    auto const saved = readCarrierSession(*runtimePtr);
    CHECK(saved.currentTrackOrdinal == 2);
    CHECK(saved.positionMs == 0);
  }

  TEST_CASE("CUE playback session - positive deferred seek clamps to a zero millisecond segment start")
  {
    auto const temp = ao::test::TempDir{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(temp, std::move(executorPtr));
    addReadyAudioProvider(*runtimePtr);
    executor->runUntilIdle();
    auto const ids = importSessionCarrier(*runtimePtr, 589);
    saveCarrierSession(*runtimePtr, sessionForCarrier(*runtimePtr, ids[1]));
    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    auto& playback = runtimePtr->playback();
    CHECK(playback.snapshot().transport.duration == std::chrono::milliseconds{0});
    auto const occurrenceId = playback.snapshot().transport.occurrenceId;

    playback.commands().seek(std::chrono::milliseconds{1});
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{0});
    CHECK(playback.snapshot().transport.occurrenceId == occurrenceId);
    CHECK(playback.snapshot().transport.transport == audio::Transport::Idle);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == ids[1]);
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK(readCarrierSession(*runtimePtr).positionMs == 0);

    playback.commands().resume();
    executor->runUntilIdle();
    CHECK(playback.snapshot().transport.transport == audio::Transport::Playing);
    CHECK(playback.snapshot().transport.nowPlaying.trackId == ids[1]);
    REQUIRE(runtimePtr->savePlaybackSession());
    auto const saved = readCarrierSession(*runtimePtr);
    CHECK(saved.currentTrackOrdinal == 2);
    CHECK(saved.positionMs == 0);
  }

  TEST_CASE("Playback session - unknown duration whole file retains restored and deferred seek offsets")
  {
    auto const temp = ao::test::TempDir{};
    auto executorPtr = std::make_unique<ManualExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(temp, std::move(executorPtr));
    // Import a whole file whose duration floors to zero. addRuntimeTrack applies
    // descriptive metadata only; technical duration comes from the media itself.
    auto const bytes = ao::test::wav::makeWav(ao::test::wav::Spec{
      .sampleRate = 44100,
      .audioData = std::vector<std::uint8_t>(2, 0x21),
    });
    auto const file = ao::test::TempFile{bytes, ".wav"};
    std::filesystem::copy_file(file.path, runtimePtr->musicRoot() / "unknown.wav");
    auto const trackId = addRuntimeTrack(*runtimePtr,
                                         library::test::TrackSpec{.title = "Unknown duration", .uri = "unknown.wav"},
                                         [executor] { executor->runUntilIdle(); });
    runtimePtr->sources().reloadAllTracks();
    auto const reader = runtimePtr->library().snapshot();
    auto const optUri = reader.trackUri(trackId);
    REQUIRE(optUri);
    REQUIRE_FALSE(reader.trackSegment(trackId));
    auto const session = PlaybackSessionState{
      .libraryId = reader.libraryId(),
      .sourceListId = kAllTracksListId,
      .currentTrackId = trackId,
      .currentTrackUri = *optUri,
      .positionMs = 1,
    };
    saveCarrierSession(*runtimePtr, session);

    auto const restoredRes = runtimePtr->restorePlaybackSession();
    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    auto& playback = runtimePtr->playback();
    CHECK(playback.snapshot().transport.duration == std::chrono::milliseconds{0});
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{1});
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK(readCarrierSession(*runtimePtr).positionMs == 1);

    playback.commands().seek(std::chrono::milliseconds{2});
    CHECK(playback.snapshot().transport.elapsed == std::chrono::milliseconds{2});
    CHECK(playback.snapshot().transport.nowPlaying.trackId == trackId);
    REQUIRE(runtimePtr->savePlaybackSession());
    auto const saved = readCarrierSession(*runtimePtr);
    CHECK(saved.currentTrackOrdinal == 0);
    CHECK(saved.positionMs == 2);
  }
} // namespace ao::rt::test
