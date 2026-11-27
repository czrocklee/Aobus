// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaGraphRegistry.h"
#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/audio/backend/AlsaMixerTestSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/AsyncTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/PlaybackTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Property.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    class AlsaMuteProvider final : public audio::BackendProvider
    {
    public:
      explicit AlsaMuteProvider(std::shared_ptr<audio::backend::detail::test::FakeMixerState> mixerStatePtr,
                                bool initialApplicationMuted = false)
        : _mixerStatePtr{std::move(mixerStatePtr)}, _initialApplicationMuted{initialApplicationMuted}
      {
      }

      void shutdown() noexcept override { _registry.shutdown(); }

      utility::ScopedRegistration subscribeDevices(OnDevicesChangedCallback callback) override
      {
        if (callback)
        {
          callback(status().devices);
        }

        return {};
      }

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& device,
                                                    audio::ProfileId const& /*profile*/) override
      {
        auto backendPtr = std::make_unique<audio::backend::detail::test::AlsaControlBackend>(
          device, _registry.publisher(), _mixerStatePtr);
        std::ignore = backendPtr->set(audio::props::kMuted, _initialApplicationMuted);
        return backendPtr;
      }

      Status status() const override
      {
        return {.descriptor = {.id = audio::kBackendAlsa, .supportedProfiles = {{.id = audio::kProfileExclusive}}},
                .devices = {{.id = audio::DeviceId{"hw:test,0"},
                             .displayName = "Test ALSA card",
                             .description = "Injected mixer",
                             .isDefault = true,
                             .backendId = audio::kBackendAlsa}}};
      }

      utility::ScopedRegistration subscribeGraph(std::string_view routeAnchor, OnGraphChangedCallback callback) override
      {
        return _registry.subscribe(routeAnchor, std::move(callback));
      }

    private:
      std::shared_ptr<audio::backend::detail::test::FakeMixerState> _mixerStatePtr;
      audio::backend::detail::AlsaGraphRegistry _registry;
      bool _initialApplicationMuted = false;
    };

    PlaybackSessionState loadStoredSession(ConfigStore& store)
    {
      auto session = PlaybackSessionState{};
      auto const loadedRes = store.load(kPlaybackSessionConfigGroup, session, PlaybackSessionYamlSchema{});
      REQUIRE(loadedRes);
      REQUIRE(*loadedRes);
      return session;
    }
  } // namespace

  TEST_CASE("PlaybackSession - external ALSA mute is not saved or restored as application intent")
  {
    auto tempDir = ao::test::TempDir{};
    auto playbackSessionStore = ConfigStore{tempDir.path() / "application.yaml"};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto ownerPtr = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore);
    auto& runtime = ownerPtr->runtime();
    auto mixerStatePtr = std::make_shared<audio::backend::detail::test::FakeMixerState>();
    mixerStatePtr->optHardwareMuted = true;
    mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
    runtime.addAudioProvider(std::make_unique<AlsaMuteProvider>(mixerStatePtr));
    executor->drain();

    auto const uri =
      audio::test::installAudioFixture(runtime.musicRoot(), "basic_metadata.flac", "alsa-mute-session.flac");
    auto const trackId = addRuntimeTrack(*ownerPtr,
                                         library::test::TrackSpec{
                                           .title = "External mute",
                                           .uri = uri,
                                           .duration = std::chrono::seconds{10},
                                         },
                                         [&] { executor->drain(); });
    runtime.sources().reloadAllTracks();
    auto const viewRes = runtime.workspace().navigate(NavigationRequest{
      .target = FilteredListTarget{.listId = kAllTracksListId, .filterExpression = {}},
    });
    REQUIRE(viewRes);
    REQUIRE(executor->tryDrainUntil([&] { return runtime.playback().snapshot().transport.ready; }));
    REQUIRE(admitPlaybackAndWait(
      *executor,
      [&] { return runtime.playback().commands().startFromView(*viewRes, trackId); },
      [&] { return runtime.playback().snapshot().transport.positionRevision; }));

    CHECK_FALSE(runtime.playback().snapshot().transport.volume.muted);
    REQUIRE(runtime.savePlaybackSession());
    CHECK_FALSE(loadStoredSession(playbackSessionStore).muted);

    runtime.playback().commands().stop();
    ownerPtr.reset();
    mixerStatePtr->optHardwareMuted = false;
    auto restoredExecutorPtr = std::make_unique<QueuedExecutor>();
    auto* const restoredExecutor = restoredExecutorPtr.get();
    auto restoredOwnerPtr = makeRuntime(tempDir, std::move(restoredExecutorPtr), &playbackSessionStore);
    auto& restoredRuntime = restoredOwnerPtr->runtime();
    restoredRuntime.addAudioProvider(std::make_unique<AlsaMuteProvider>(mixerStatePtr, true));
    restoredExecutor->drain();
    REQUIRE(restoredRuntime.playback().snapshot().transport.volume.muted);
    CHECK(mixerStatePtr->writeCount == 0U);

    auto const restoredRes = restoredRuntime.restorePlaybackSession();
    restoredExecutor->drain();

    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    CHECK_FALSE(restoredRuntime.playback().snapshot().transport.volume.muted);
    // The preinitialized fake mixer receives restore's explicit volume request, not a mute-switch write.
    CHECK(mixerStatePtr->writeCount == 1U);
  }

  TEST_CASE("PlaybackSession - ALSA hardware volume survives stop checkpoint shutdown and restore exactly")
  {
    auto tempDir = ao::test::TempDir{};
    auto const configPath = tempDir.path() / "application.yaml";
    auto playbackSessionStore = ConfigStore{configPath};
    auto sleeper = ControlledSleeper{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto ownerPtr = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore, &sleeper);
    auto& runtime = ownerPtr->runtime();
    auto mixerStatePtr = std::make_shared<audio::backend::detail::test::FakeMixerState>();
    mixerStatePtr->hardwareElements.push_back(
      {.id = {.name = "PCM", .index = 0U}, .rawRange = {.min = 0L, .max = 100L}, .rawLevels = {100L}});
    runtime.addAudioProvider(std::make_unique<AlsaMuteProvider>(mixerStatePtr));
    executor->drain();

    auto const uri =
      audio::test::installAudioFixture(runtime.musicRoot(), "basic_metadata.flac", "alsa-volume-session.flac");
    auto const trackId = addRuntimeTrack(*ownerPtr,
                                         library::test::TrackSpec{
                                           .title = "Hardware volume intent",
                                           .uri = uri,
                                           .duration = std::chrono::seconds{10},
                                         },
                                         [&] { executor->drain(); });
    runtime.sources().reloadAllTracks();
    auto const viewRes = runtime.workspace().navigate(NavigationRequest{
      .target = FilteredListTarget{.listId = kAllTracksListId, .filterExpression = {}},
    });
    REQUIRE(viewRes);
    REQUIRE(executor->tryDrainUntil([&] { return runtime.playback().snapshot().transport.ready; }));
    REQUIRE(admitPlaybackAndWait(
      *executor,
      [&] { return runtime.playback().commands().startFromView(*viewRes, trackId); },
      [&] { return runtime.playback().snapshot().transport.positionRevision; }));
    runtime.playback().commands().pause();
    runtime.playback().commands().seek(std::chrono::milliseconds{450});
    runtime.playback().commands().setVolume(0.25F);
    runtime.playback().commands().setMuted(true);
    executor->drain();
    REQUIRE(runtime.playback().snapshot().transport.volume.level == 0.25F);
    REQUIRE(runtime.playback().snapshot().transport.volume.hardwareAssisted);
    REQUIRE(mixerStatePtr->writeCount == 1U);
    REQUIRE(runtime.savePlaybackSession());
    auto const expected = loadStoredSession(playbackSessionStore);
    REQUIRE(expected.currentTrackId == trackId);
    REQUIRE(expected.positionMs == 450U);
    REQUIRE(expected.volume == 0.25F);
    REQUIRE(expected.muted);
    runtime.startPlaybackSessionPersistence();

    runtime.playback().commands().stop();
    executor->drain();

    // The pre-stop restorable transport cache must not hide a wrong public output snapshot.
    auto const stoppedVolume = runtime.playback().snapshot().transport.volume;
    CHECK(stoppedVolume.level == 0.25F);
    CHECK(stoppedVolume.muted);
    CHECK_FALSE(stoppedVolume.available);
    CHECK_FALSE(stoppedVolume.hardwareAssisted);
    CHECK(loadStoredSession(playbackSessionStore) == expected);
    REQUIRE(runtime.savePlaybackSession());
    auto checkpointStore = ConfigStore{configPath};
    CHECK(loadStoredSession(checkpointStore) == expected);
    CHECK(mixerStatePtr->hardwareElements.front().rawLevels == std::vector<long>{25L});
    CHECK(mixerStatePtr->writeCount == 1U);

    // A different on-disk value proves shutdown rewrites the checkpoint rather than leaving it untouched.
    auto sentinel = expected;
    sentinel.volume = 0.75F;
    sentinel.muted = false;
    REQUIRE(playbackSessionStore.save(kPlaybackSessionConfigGroup, sentinel, PlaybackSessionYamlSchema{}));
    ownerPtr->shutdown();
    auto shutdownStore = ConfigStore{configPath};
    CHECK(loadStoredSession(shutdownStore) == expected);
    CHECK(mixerStatePtr->writeCount == 1U);
    ownerPtr.reset();

    auto restoredExecutorPtr = std::make_unique<QueuedExecutor>();
    auto* const restoredExecutor = restoredExecutorPtr.get();
    auto restoredOwnerPtr = makeRuntime(tempDir, std::move(restoredExecutorPtr), &shutdownStore, &sleeper);
    auto& restoredRuntime = restoredOwnerPtr->runtime();
    restoredRuntime.addAudioProvider(std::make_unique<AlsaMuteProvider>(mixerStatePtr));
    restoredExecutor->drain();
    auto const restoredRes = restoredRuntime.restorePlaybackSession();
    restoredExecutor->drain();

    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    CHECK(restoredRuntime.playback().snapshot().transport.volume.level == 0.25F);
    CHECK(restoredRuntime.playback().snapshot().transport.volume.muted);
    CHECK(mixerStatePtr->hardwareElements.front().rawLevels == std::vector<long>{25L});
    CHECK((mixerStatePtr->writtenLevels == std::vector<long>{25L, 25L}));
    CHECK(mixerStatePtr->writeCount == 2U);
    REQUIRE(restoredRuntime.savePlaybackSession());
    auto restoredStore = ConfigStore{configPath};
    CHECK(loadStoredSession(restoredStore) == expected);
  }
} // namespace ao::rt::test
