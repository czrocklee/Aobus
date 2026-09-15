// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaGraphRegistry.h"
#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/audio/backend/AlsaMixerTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/PlaybackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
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

  TEST_CASE("PlaybackSession - external ALSA mute is not saved or restored as application intent",
            "[runtime][regression][playback-session]")
  {
    auto tempDir = ao::test::TempDir{};
    auto playbackSessionStore = ConfigStore{tempDir.path() / "application.yaml"};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr), &playbackSessionStore);
    auto mixerStatePtr = std::make_shared<audio::backend::detail::test::FakeMixerState>();
    mixerStatePtr->optHardwareMuted = true;
    mixerStatePtr->hardwareElements.push_back({.id = {.name = "PCM", .index = 0U}, .rawLevels = {80L}});
    runtimePtr->addAudioProvider(std::make_unique<AlsaMuteProvider>(mixerStatePtr));
    executor->drain();

    auto const uri =
      audio::test::installAudioFixture(runtimePtr->musicRoot(), "basic_metadata.flac", "alsa-mute-session.flac");
    auto const trackId = addRuntimeTrack(*runtimePtr,
                                         library::test::TrackSpec{
                                           .title = "External mute",
                                           .uri = uri,
                                           .duration = std::chrono::seconds{10},
                                         },
                                         [&] { executor->drain(); });
    runtimePtr->sources().reloadAllTracks();
    auto const viewRes = runtimePtr->workspace().navigate(NavigationRequest{
      .target = FilteredListTarget{.listId = kAllTracksListId, .filterExpression = {}},
    });
    REQUIRE(viewRes);
    REQUIRE(executor->tryDrainUntil([&] { return runtimePtr->playback().snapshot().transport.ready; }));
    REQUIRE(admitPlaybackAndWait(
      *executor,
      [&] { return runtimePtr->playback().commands().startFromView(*viewRes, trackId); },
      [&] { return runtimePtr->playback().snapshot().transport.positionRevision; }));

    CHECK_FALSE(runtimePtr->playback().snapshot().transport.volume.muted);
    REQUIRE(runtimePtr->savePlaybackSession());
    CHECK_FALSE(loadStoredSession(playbackSessionStore).muted);

    runtimePtr->playback().commands().stop();
    runtimePtr.reset();
    mixerStatePtr->optHardwareMuted = false;
    auto restoredExecutorPtr = std::make_unique<QueuedExecutor>();
    auto* const restoredExecutor = restoredExecutorPtr.get();
    auto restoredRuntimePtr = makeRuntime(tempDir, std::move(restoredExecutorPtr), &playbackSessionStore);
    restoredRuntimePtr->addAudioProvider(std::make_unique<AlsaMuteProvider>(mixerStatePtr, true));
    restoredExecutor->drain();
    REQUIRE(restoredRuntimePtr->playback().snapshot().transport.volume.muted);
    CHECK(mixerStatePtr->writeCount == 0U);

    auto const restoredRes = restoredRuntimePtr->restorePlaybackSession();
    restoredExecutor->drain();

    REQUIRE(restoredRes);
    REQUIRE(restoredRes->restored);
    CHECK_FALSE(restoredRuntimePtr->playback().snapshot().transport.volume.muted);
    // The preinitialized fake mixer receives restore's explicit volume request, not a mute-switch write.
    CHECK(mixerStatePtr->writeCount == 1U);
  }
} // namespace ao::rt::test
