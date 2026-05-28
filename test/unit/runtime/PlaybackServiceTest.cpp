// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestUtils.h"
#include "test/unit/audio/TestUtility.h"
#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/IRenderTarget.h>
#include <ao/audio/Property.h>
#include <ao/audio/Types.h>
#include <ao/audio/flow/Graph.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/async/Runtime.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    struct NullExecutor final : public IControlExecutor
    {
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };
  }

  TEST_CASE("PlaybackService - Basic Flow", "[app][unit][runtime][playback]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};
    auto listSourceStore = ListSourceStore{testLib.library(), mutationService};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};

    auto playbackService = PlaybackService{executor, viewService, testLib.library()};

    auto mockProvider = fakeit::Mock<audio::IBackendProvider>{};
    auto onDevicesChangedCb = audio::IBackendProvider::OnDevicesChangedCallback{};
    fakeit::When(Method(mockProvider, subscribeDevices))
      .AlwaysDo(
        [&](audio::IBackendProvider::OnDevicesChangedCallback cb)
        {
          onDevicesChangedCb = cb;
          return audio::Subscription{};
        });

    auto onGraphChangedCb = audio::IBackendProvider::OnGraphChangedCallback{};
    fakeit::When(Method(mockProvider, subscribeGraph))
      .AlwaysDo(
        [&](std::string_view, audio::IBackendProvider::OnGraphChangedCallback cb)
        {
          onGraphChangedCb = cb;
          return audio::Subscription{};
        });

    auto status = audio::IBackendProvider::Status{};
    status.metadata.id = audio::BackendId{"mock_backend"};
    status.metadata.name = "Mock Backend";
    status.devices.push_back(audio::Device{.id = audio::DeviceId{"mock_device"},
                                           .displayName = "Mock Device",
                                           .description = "A mock audio device",
                                           .isDefault = true,
                                           .backendId = audio::BackendId{"mock_backend"}});
    status.metadata.supportedProfiles.push_back(audio::IBackendProvider::ProfileMetadata{
      .id = audio::ProfileId{audio::kProfileShared}, .name = "Shared", .description = "Shared profile"});

    fakeit::When(Method(mockProvider, status)).AlwaysReturn(status);

    auto spyBackend = std::make_shared<audio::test::SpyBackend<>>();
    fakeit::When(Method(spyBackend->mock(), property))
      .AlwaysDo(
        [](audio::PropertyId id) -> Result<audio::PropertyValue>
        {
          if (id == audio::PropertyId::Volume)
          {
            return 1.0F;
          }

          if (id == audio::PropertyId::Muted)
          {
            return false;
          }

          return 0.0F;
        });
    fakeit::When(Method(spyBackend->mock(), queryProperty)).AlwaysReturn(audio::PropertyInfo{});
    fakeit::When(Method(spyBackend->mock(), backendId)).AlwaysReturn(audio::BackendId{"mock_backend"});
    fakeit::When(Method(spyBackend->mock(), profileId)).AlwaysReturn(audio::ProfileId{audio::kProfileShared});

    audio::IRenderTarget* renderTarget = nullptr;
    fakeit::When(Method(spyBackend->mock(), open))
      .AlwaysDo(
        [&](audio::Format const& /*format*/, audio::IRenderTarget* target) -> Result<>
        {
          renderTarget = target;
          return {};
        });

    // We must return a unique_ptr to IBackend from createBackend
    fakeit::When(Method(mockProvider, createBackend))
      .AlwaysDo([&](audio::Device const&, audio::ProfileId const&) { return spyBackend->makeProxy(); });

    playbackService.addProvider(std::make_unique<audio::test::MockProviderProxy>(mockProvider.get()));

    if (onDevicesChangedCb)
    {
      onDevicesChangedCb(status.devices);
      // Trigger again to hit the early return when selectedOutput is already set
      onDevicesChangedCb(status.devices);

      // Trigger with a backend that has no devices
      auto emptyStatus = status;
      emptyStatus.devices.clear();
      onDevicesChangedCb(emptyStatus.devices);
    }

    SECTION("Initial state is correct")
    {
      auto const& state = playbackService.state();
      CHECK(state.trackId == kInvalidTrackId);
      CHECK(state.sourceListId == kInvalidListId);
      CHECK(state.durationMs == 0);
      CHECK(state.positionMs == 0);
      CHECK(state.shuffleMode == ShuffleMode::Off);
      CHECK(state.repeatMode == RepeatMode::Off);
    }

    SECTION("Control methods update state and emit signals")
    {
      bool shuffleChanged = false;
      auto sub1 = playbackService.onShuffleModeChanged(
        [&](auto const& ev)
        {
          shuffleChanged = true;
          CHECK(ev.mode == ShuffleMode::On);
        });

      playbackService.setShuffleMode(ShuffleMode::On);
      CHECK(shuffleChanged);
      CHECK(playbackService.state().shuffleMode == ShuffleMode::On);

      bool repeatChanged = false;
      auto sub2 = playbackService.onRepeatModeChanged(
        [&](auto const& ev)
        {
          repeatChanged = true;
          CHECK(ev.mode == RepeatMode::All);
        });

      playbackService.setRepeatMode(RepeatMode::All);
      CHECK(repeatChanged);
      CHECK(playbackService.state().repeatMode == RepeatMode::All);
    }

    SECTION("Playback control (play, pause, resume, stop)")
    {
      bool preparingFired = false;
      auto subPrep = playbackService.onPreparing([&] { preparingFired = true; });

      bool startedFired = false;
      auto subStart = playbackService.onStarted([&] { startedFired = true; });

      bool nowPlayingFired = false;
      auto subNow = playbackService.onNowPlayingChanged(
        [&](auto const& ev)
        {
          if (ev.trackId == TrackId{1})
          {
            nowPlayingFired = true;
          }
        });

      auto const desc = audio::TrackPlaybackDescriptor{
        .trackId = TrackId{1},
        .filePath = "/fake/path.flac",
        .title = "Fake Track",
        .artist = "Fake Artist",
        .durationMs = 120000,
      };

      playbackService.play(desc, ListId{1});

      CHECK(preparingFired);
      CHECK(startedFired);
      CHECK(nowPlayingFired);
      CHECK(playbackService.state().trackId == TrackId{1});
      CHECK(playbackService.state().sourceListId == ListId{1});
      CHECK(playbackService.state().trackTitle == "Fake Track");
      CHECK(playbackService.state().trackArtist == "Fake Artist");

      // Pause
      bool pausedFired = false;
      auto subPause = playbackService.onPaused([&] { pausedFired = true; });
      playbackService.pause();
      CHECK(pausedFired);

      // Resume
      startedFired = false;
      playbackService.resume();
      CHECK(startedFired);

      // Stop
      bool stoppedFired = false;
      auto subStop = playbackService.onStopped([&] { stoppedFired = true; });
      bool idleFired = false;
      auto subIdle = playbackService.onIdle([&] { idleFired = true; });

      playbackService.stop();
      CHECK(stoppedFired);
      CHECK(idleFired);
      CHECK(playbackService.state().trackId == kInvalidTrackId);
    }

    SECTION("Seek updates state and fires event")
    {
      bool seekFired = false;
      auto sub = playbackService.onSeekUpdate(
        [&](auto const& ev)
        {
          seekFired = true;
          CHECK(ev.positionMs == 1000);
          CHECK(ev.mode == PlaybackService::SeekMode::Final);
        });

      playbackService.seek(1000, PlaybackService::SeekMode::Final);
      CHECK(seekFired);
    }

    SECTION("Volume and Muted controls update state")
    {
      playbackService.setVolume(0.5F);
      CHECK(playbackService.state().volume == 0.5F);

      playbackService.setMuted(true);
      CHECK(playbackService.state().muted == true);
    }

    SECTION("Reveal track requests")
    {
      bool revealFired = false;
      auto sub = playbackService.onRevealTrackRequested(
        [&](auto const& ev)
        {
          revealFired = true;
          CHECK(ev.trackId == TrackId{42});
        });
      playbackService.revealTrack(TrackId{42});
      CHECK(revealFired);
    }

    SECTION("Signal subscriptions (devices, output, quality)")
    {
      bool devicesChangedFired = false;
      auto sub1 = playbackService.onDevicesChanged([&] { devicesChangedFired = true; });

      bool outputChangedFired = false;
      auto sub2 = playbackService.onOutputChanged(
        [&](auto const& ev)
        {
          outputChangedFired = true;
          CHECK(ev.backendId == audio::BackendId{"mock_backend"});
        });

      bool qualityChangedFired = false;
      auto sub3 = playbackService.onQualityChanged(
        [&](auto const& ev)
        {
          qualityChangedFired = true;
          CHECK(ev.ready == true);
        });

      playbackService.setOutput(
        audio::BackendId{"mock_backend"}, audio::DeviceId{"mock_device"}, audio::ProfileId{audio::kProfileShared});
      CHECK(outputChangedFired);
    }

    SECTION("playSelectionInView fails with empty selection")
    {
      auto const result = viewService.createView({.listId = kInvalidListId}, true);
      TrackId const tid = playbackService.playSelectionInView(result.viewId);
      CHECK(tid == kInvalidTrackId);
    }

    SECTION("playSelectionInView fails when track does not exist")
    {
      auto const result = viewService.createView({.listId = kInvalidListId}, true);
      viewService.setSelection(result.viewId, {TrackId{99999}});

      TrackId const tid = playbackService.playSelectionInView(result.viewId);
      CHECK(tid == kInvalidTrackId);
    }

    SECTION("playSelectionInView fails with invalid view or selection")
    {
      TrackId const tid = playbackService.playSelectionInView(ViewId{999});
      CHECK(tid == kInvalidTrackId);
    }

    SECTION("playSelectionInView succeeds")
    {
      auto const trackId = testLib.addTrack("A Track");
      auto const result = viewService.createView({.listId = kInvalidListId}, true);
      viewService.setSelection(result.viewId, {trackId});

      if (onGraphChangedCb)
      {
        onGraphChangedCb(audio::flow::Graph{});
      }

      TrackId const tid = playbackService.playSelectionInView(result.viewId);
      CHECK(tid == trackId);
      CHECK(playbackService.state().trackId == trackId);

      if (renderTarget != nullptr)
      {
        renderTarget->onRouteReady("mock_anchor");
        renderTarget->onDrainComplete();
        renderTarget->onPropertyChanged(audio::PropertyId::Volume);
      }
    }

    SECTION("ensureReady auto-configures output on first play")
    {
      // Create a fresh PlaybackService without triggering onDevicesChanged
      auto freshExecutor = NullExecutor{};
      auto freshRuntime = async::Runtime{freshExecutor};
      auto freshMutation = LibraryMutationService{freshRuntime, testLib.library()};
      auto freshListStore = ListSourceStore{testLib.library(), freshMutation};
      auto freshViewService = ViewService{freshExecutor, testLib.library(), freshListStore};
      auto freshService = PlaybackService{freshExecutor, freshViewService, testLib.library()};

      auto freshMockProvider = fakeit::Mock<audio::IBackendProvider>{};

      // Capture the devices-changed callback but do NOT invoke it
      auto freshDevicesCb = audio::IBackendProvider::OnDevicesChangedCallback{};
      fakeit::When(Method(freshMockProvider, subscribeDevices))
        .AlwaysDo(
          [&](audio::IBackendProvider::OnDevicesChangedCallback cb)
          {
            freshDevicesCb = cb;
            return audio::Subscription{};
          });

      fakeit::When(Method(freshMockProvider, subscribeGraph))
        .AlwaysDo([](std::string_view, audio::IBackendProvider::OnGraphChangedCallback)
                  { return audio::Subscription{}; });

      fakeit::When(Method(freshMockProvider, status)).AlwaysReturn(status);

      auto freshSpyBackend = std::make_shared<audio::test::SpyBackend<>>();
      fakeit::When(Method(freshSpyBackend->mock(), property))
        .AlwaysDo(
          [](audio::PropertyId id) -> Result<audio::PropertyValue>
          {
            if (id == audio::PropertyId::Volume)
            {
              return 1.0F;
            }

            if (id == audio::PropertyId::Muted)
            {
              return false;
            }

            return 0.0F;
          });
      fakeit::When(Method(freshSpyBackend->mock(), queryProperty)).AlwaysReturn(audio::PropertyInfo{});
      fakeit::When(Method(freshSpyBackend->mock(), backendId)).AlwaysReturn(audio::BackendId{"mock_backend"});
      fakeit::When(Method(freshSpyBackend->mock(), profileId)).AlwaysReturn(audio::ProfileId{audio::kProfileShared});
      fakeit::When(Method(freshSpyBackend->mock(), open))
        .AlwaysDo([](audio::Format const&, audio::IRenderTarget*) -> Result<> { return {}; });
      fakeit::When(Method(freshMockProvider, createBackend))
        .AlwaysDo([&](audio::Device const&, audio::ProfileId const&) { return freshSpyBackend->makeProxy(); });

      freshService.addProvider(std::make_unique<audio::test::MockProviderProxy>(freshMockProvider.get()));

      // Do NOT call freshDevicesCb — player is not ready yet.
      // Calling play() should trigger ensureReady() which auto-configures the output.
      auto const desc = audio::TrackPlaybackDescriptor{
        .trackId = TrackId{1},
        .filePath = "/fake/path.flac",
        .title = "Fake Track",
        .artist = "Fake Artist",
        .durationMs = 120000,
      };

      freshService.play(desc, ListId{1});
      CHECK(freshService.state().trackId == TrackId{1});
    }

    SECTION("revealPlayingTrack works")
    {
      bool revealFired = false;
      auto sub = playbackService.onRevealTrackRequested([&](PlaybackService::RevealTrackRequested const& /*ev*/)
                                                        { revealFired = true; });
      playbackService.revealPlayingTrack();
      CHECK(revealFired);
    }
  }
} // namespace ao::rt::test
