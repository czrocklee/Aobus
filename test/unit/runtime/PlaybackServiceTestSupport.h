// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/audio/TestUtility.h"
#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Format.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/Property.h>
#include <ao/audio/Types.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>

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

namespace ao::audio
{
  class IRenderTarget;
}

namespace ao::rt::test
{
  inline PlaybackService::PlaybackRequest playbackRequest(TrackId trackId,
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
  inline audio::IBackendProvider::Status makeMockProviderStatus()
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
  // notify devices: each test triggers onDevicesChangedCb itself because the call
  // sites need different priming (auto-select-and-edge-cases, a single
  // notify-then-drain, or no notify at all to exercise ensureReady()).
  template<typename ExecutorT>
  struct PlaybackFixture final
  {
    PlaybackFixture()
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

    PlaybackFixture(PlaybackFixture const&) = delete;
    PlaybackFixture& operator=(PlaybackFixture const&) = delete;
    PlaybackFixture(PlaybackFixture&&) = delete;
    PlaybackFixture& operator=(PlaybackFixture&&) = delete;
    ~PlaybackFixture() = default;

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
} // namespace ao::rt::test
