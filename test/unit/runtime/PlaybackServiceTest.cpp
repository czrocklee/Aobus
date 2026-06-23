// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
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
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    PlaybackService::PlaybackRequest playbackRequest(TrackId trackId,
                                                     std::string_view filePath,
                                                     std::string title,
                                                     std::string artist,
                                                     std::chrono::milliseconds duration)
    {
      return PlaybackService::PlaybackRequest{
        .trackId = trackId,
        .input = audio::PlaybackInput{.filePath = std::string{filePath}, .duration = duration},
        .title = std::move(title),
        .artist = std::move(artist),
      };
    }

    // Canonical single-backend, single-device provider status shared by every
    // harness instance below: "mock_backend" exposes one default "mock_device"
    // and the shared profile.
    audio::IBackendProvider::Status makeMockProviderStatus()
    {
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
      return status;
    }
  } // namespace

  // Executor that queues tasks instead of running them inline, so a test can
  // drive the executor turn boundary explicitly via drain(). isCurrent() answers
  // truthfully so the PlaybackService affinity guard is exercised on a real
  // thread-id comparison rather than the unconditional-true MockExecutor.
  class QueuedExecutor final : public async::IExecutor
  {
  public:
    bool isCurrent() const noexcept override { return std::this_thread::get_id() == _ownerThreadId; }

    void dispatch(std::move_only_function<void()> task) override
    {
      auto const lock = std::scoped_lock{_mutex};
      _tasks.push_back(std::move(task));
    }

    void defer(std::move_only_function<void()> task) override { dispatch(std::move(task)); }

    void drain()
    {
      while (true)
      {
        auto task = std::move_only_function<void()>{};
        {
          auto const lock = std::scoped_lock{_mutex};

          if (_tasks.empty())
          {
            return;
          }

          task = std::move(_tasks.front());
          _tasks.pop_front();
        }

        if (task)
        {
          task();
        }
      }
    }

  private:
    std::thread::id _ownerThreadId = std::this_thread::get_id();
    std::deque<std::move_only_function<void()>> _tasks;
    std::mutex _mutex;
  };

  // Shared wiring for the PlaybackService tests: a music library, a view service,
  // a spy backend, and a mocked IBackendProvider that hands out that backend.
  // ExecutorT selects the dispatch model (MockExecutor runs inline; QueuedExecutor
  // defers until drain()). The provider's devices/graph callbacks and the render
  // target are captured into public members so a test can drive them.
  //
  // The constructor wires the mocks and registers the provider, but it does NOT
  // notify devices: each test triggers onDevicesChangedCb itself because the three
  // call sites need different priming (auto-select-and-edge-cases, a single
  // notify-then-drain, or no notify at all to exercise ensureReady()).
  template<typename ExecutorT>
  struct PlaybackHarness final
  {
    PlaybackHarness()
    {
      fakeit::Fake(Method(mockProvider, shutdown));

      fakeit::When(Method(mockProvider, subscribeDevices))
        .AlwaysDo(
          [this](audio::IBackendProvider::OnDevicesChangedCallback cb)
          {
            onDevicesChangedCb = cb;
            return audio::Subscription{};
          });

      fakeit::When(Method(mockProvider, subscribeGraph))
        .AlwaysDo(
          [this](std::string_view, audio::IBackendProvider::OnGraphChangedCallback cb)
          {
            onGraphChangedCb = cb;
            return audio::Subscription{};
          });

      fakeit::When(Method(mockProvider, status)).AlwaysReturn(status);

      fakeit::When(Method(spyBackendPtr->mock(), property))
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
      fakeit::When(Method(spyBackendPtr->mock(), queryProperty))
        .AlwaysReturn(audio::PropertyInfo{
          .canRead = true,
          .canWrite = true,
          .isAvailable = true,
          .emitsChangeNotifications = false,
          .isHardwareAssisted = true,
        });
      fakeit::When(Method(spyBackendPtr->mock(), backendId)).AlwaysReturn(audio::BackendId{"mock_backend"});
      fakeit::When(Method(spyBackendPtr->mock(), profileId)).AlwaysReturn(audio::ProfileId{audio::kProfileShared});
      fakeit::When(Method(spyBackendPtr->mock(), open))
        .AlwaysDo(
          [this](audio::Format const& /*format*/, audio::IRenderTarget* target) -> Result<>
          {
            renderTarget = target;
            return {};
          });
      fakeit::When(Method(mockProvider, createBackend))
        .AlwaysDo([this](audio::Device const&, audio::ProfileId const&) { return spyBackendPtr->makeProxy(); });

      playbackService.addProvider(std::make_unique<audio::test::MockProviderProxy>(mockProvider.get()));
    }

    PlaybackHarness(PlaybackHarness const&) = delete;
    PlaybackHarness& operator=(PlaybackHarness const&) = delete;
    PlaybackHarness(PlaybackHarness&&) = delete;
    PlaybackHarness& operator=(PlaybackHarness&&) = delete;
    ~PlaybackHarness() = default;

    // Declaration order matters: the executor must outlive the view/playback
    // services that hold references to it, and playbackService (destroyed first)
    // tears down its Player while the provider mock is still alive.
    TestMusicLibrary testLib;
    ExecutorT executor;
    LibraryChanges changes;
    ListSourceStore listSourceStore{testLib.library(), changes};
    ViewService viewService{executor, testLib.library(), listSourceStore};

    std::shared_ptr<audio::test::SpyBackend<>> spyBackendPtr = std::make_shared<audio::test::SpyBackend<>>();
    fakeit::Mock<audio::IBackendProvider> mockProvider;
    audio::IBackendProvider::Status status = makeMockProviderStatus();

    audio::IBackendProvider::OnDevicesChangedCallback onDevicesChangedCb;
    audio::IBackendProvider::OnGraphChangedCallback onGraphChangedCb;
    audio::IRenderTarget* renderTarget = nullptr;

    PlaybackService playbackService{executor, viewService, testLib.library()};
  };

  TEST_CASE("PlaybackService - Basic Flow", "[app][unit][runtime][playback]")
  {
    auto h = PlaybackHarness<MockExecutor>{};
    auto& playbackService = h.playbackService;
    auto& viewService = h.viewService;
    auto& testLib = h.testLib;
    auto*& renderTarget = h.renderTarget;
    auto& onGraphChangedCb = h.onGraphChangedCb;

    // Prime the device list. The first notification auto-selects the default
    // output; the duplicate exercises the "already selected" early return, and the
    // empty list exercises the no-devices guard.
    h.onDevicesChangedCb(h.status.devices);
    h.onDevicesChangedCb(h.status.devices);
    auto emptyStatus = h.status;
    emptyStatus.devices.clear();
    h.onDevicesChangedCb(emptyStatus.devices);

    SECTION("Initial state is correct")
    {
      auto const& state = playbackService.state();
      CHECK(state.trackId == kInvalidTrackId);
      CHECK(state.sourceListId == kInvalidListId);
      CHECK(state.duration == std::chrono::milliseconds{0});
      CHECK(state.elapsed == std::chrono::milliseconds{0});
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

      auto const desc =
        playbackRequest(TrackId{1}, "/fake/path.flac", "Fake Track", "Fake Artist", std::chrono::minutes{2});

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
          CHECK(ev.elapsed == std::chrono::seconds{1});
          CHECK(ev.mode == PlaybackService::SeekMode::Final);
        });

      playbackService.seek(std::chrono::seconds{1}, PlaybackService::SeekMode::Final);
      CHECK(seekFired);
    }

    SECTION("Volume and Muted controls update state")
    {
      bool mutedChangedFired = false;
      bool lastMutedState = false;
      auto sub = playbackService.onMutedChanged(
        [&](bool const muted)
        {
          mutedChangedFired = true;
          lastMutedState = muted;
        });

      playbackService.setVolume(0.5F);
      CHECK(playbackService.state().volume == 0.5F);
      CHECK(playbackService.state().volumeIsHardwareAssisted == true);

      playbackService.setMuted(true);
      CHECK(playbackService.state().muted == true);
      CHECK(mutedChangedFired);
      CHECK(lastMutedState == true);
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
      auto lastOutput = OutputSelection{};
      auto sub2 = playbackService.onOutputChanged(
        [&](auto const& ev)
        {
          outputChangedFired = true;
          lastOutput = ev;
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
      // setOutput publishes the engine-confirmed selection taken from the
      // refreshed state, not the raw request, so the emitted event mirrors
      // state().selectedOutput exactly (and stays consistent with the
      // auto-select path that also emits state.selectedOutput).
      CHECK(lastOutput.backendId == audio::BackendId{"mock_backend"});
      CHECK(lastOutput.deviceId == audio::DeviceId{"mock_device"});
      CHECK(lastOutput.profileId == audio::ProfileId{audio::kProfileShared});
      CHECK(lastOutput == playbackService.state().selectedOutput);
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

    SECTION("playTrack fails when track does not exist")
    {
      CHECK_FALSE(playbackService.playTrack(TrackId{99999}, ListId{7}));
    }

    SECTION("playTrack resolves track metadata")
    {
      auto spec = TrackSpec{};
      spec.title = "Playable Track";
      spec.artist = "Queue Artist";
      spec.album = "Queue Album";
      spec.duration = std::chrono::minutes{3};
      auto const trackId = testLib.addTrack(spec);

      CHECK(playbackService.playTrack(trackId, ListId{7}));
      CHECK(playbackService.state().trackId == trackId);
      CHECK(playbackService.state().sourceListId == ListId{7});
      CHECK(playbackService.state().trackTitle == "Playable Track");
      CHECK(playbackService.state().trackArtist == "Queue Artist");
      CHECK(playbackService.state().duration == std::chrono::minutes{3});
    }

    SECTION("ensureReady auto-configures output on first play")
    {
      // A second harness whose device list is never primed: the player is not
      // ready, so play() must run ensureReady() and auto-configure the first
      // available output before starting.
      auto fresh = PlaybackHarness<MockExecutor>{};

      auto const desc =
        playbackRequest(TrackId{1}, "/fake/path.flac", "Fake Track", "Fake Artist", std::chrono::minutes{2});

      fresh.playbackService.play(desc, ListId{1});
      CHECK(fresh.playbackService.state().trackId == TrackId{1});
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

  TEST_CASE("PlaybackService - control commands refresh state synchronously", "[app][unit][runtime][playback]")
  {
    auto h = PlaybackHarness<QueuedExecutor>{};

    // The provider callback is marshalled onto the executor, so the auto-select
    // does not take effect until the queued task drains.
    h.onDevicesChangedCb(h.status.devices);
    h.executor.drain();
    REQUIRE(h.playbackService.state().ready);

    bool startedFired = false;
    auto subStarted = h.playbackService.onStarted([&] { startedFired = true; });
    auto const desc =
      playbackRequest(TrackId{77}, "/fake/path.flac", "Deferred Track", "Deferred Artist", std::chrono::minutes{4});

    h.playbackService.play(desc, ListId{9});

    // Control commands run on the executor thread (affinity contract), so they
    // refresh the state snapshot and emit their signals synchronously — no
    // executor turn is required, unlike the async Player callbacks.
    CHECK(startedFired);
    CHECK(h.playbackService.state().trackId == TrackId{77});
    CHECK(h.playbackService.state().sourceListId == ListId{9});

    // Draining any executor-deferred Player state callbacks leaves it intact.
    h.executor.drain();

    CHECK(h.playbackService.state().trackId == TrackId{77});
    CHECK(h.playbackService.state().sourceListId == ListId{9});
  }
} // namespace ao::rt::test
