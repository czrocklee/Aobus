// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/PlaybackSessionState.h"
#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/audio/BackendTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Player.h>
#include <ao/audio/Property.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewIds.h>

#include <fakeit.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::audio
{
  class RenderTarget;
}

namespace ao::rt::test
{
  inline PlaybackService::PlaybackRequest playbackRequest(TrackId trackId,
                                                          std::string_view filePath,
                                                          std::string title,
                                                          std::string artist,
                                                          std::chrono::milliseconds duration,
                                                          std::string album = {},
                                                          ResourceId coverArtId = kInvalidResourceId,
                                                          ViewId sourceViewId = kInvalidViewId)
  {
    return PlaybackService::PlaybackRequest{
      .item = NowPlayingInfo{.trackId = trackId,
                             .sourceViewId = sourceViewId,
                             .coverArtId = coverArtId,
                             .title = std::move(title),
                             .artist = std::move(artist),
                             .album = std::move(album)},
      .input = audio::PlaybackInput{.filePath = std::string{filePath}, .duration = duration},
    };
  }

  // Canonical single-backend, single-device provider status shared by every
  // harness instance below: "mock_backend" exposes one default "mock_device"
  // and the shared profile.
  inline audio::BackendProvider::Status makeMockProviderStatus()
  {
    auto status = audio::BackendProvider::Status{};
    status.descriptor.id = audio::BackendId{"mock_backend"};
    status.descriptor.name = "Mock Backend";
    status.devices.push_back(audio::Device{.id = audio::DeviceId{"mock_device"},
                                           .displayName = "Mock Device",
                                           .description = "A mock audio device",
                                           .isDefault = true,
                                           .backendId = audio::BackendId{"mock_backend"}});
    status.descriptor.supportedProfiles.push_back(audio::BackendProvider::ProfileDescriptor{
      .id = audio::ProfileId{audio::kProfileShared}, .name = "Shared", .description = "Shared profile"});
    return status;
  }

  using rt::test::QueuedExecutor;

  // Shared wiring for the PlaybackService tests: a music library, a spy backend,
  // and a mocked BackendProvider that hands out that backend.
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
          [this](audio::BackendProvider::OnDevicesChangedCallback cb)
          {
            onDevicesChangedCb = cb;
            return audio::Subscription{};
          });

      fakeit::When(Method(mockProvider, subscribeGraph))
        .AlwaysDo(
          [this](std::string_view, audio::BackendProvider::OnGraphChangedCallback cb)
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
          [this](audio::Format const& /*format*/, audio::RenderTarget* target) -> Result<>
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

    // Declaration order matters: the executor must outlive PlaybackService,
    // and playbackService (destroyed first)
    // tears down its Player while the provider mock is still alive.
    MusicLibraryFixture libraryFixture;
    ExecutorT executor;
    NotificationService notificationService;

    std::shared_ptr<audio::test::SpyBackend<>> spyBackendPtr = std::make_shared<audio::test::SpyBackend<>>();
    fakeit::Mock<audio::BackendProvider> mockProvider;
    audio::BackendProvider::Status status = makeMockProviderStatus();

    audio::BackendProvider::OnDevicesChangedCallback onDevicesChangedCb;
    audio::BackendProvider::OnGraphChangedCallback onGraphChangedCb;
    audio::RenderTarget* renderTarget = nullptr;

    PlaybackService playbackService{executor,
                                    libraryFixture.library(),
                                    notificationService,
                                    std::make_unique<audio::Player>(executor)};
  };
} // namespace ao::rt::test
