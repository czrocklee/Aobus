// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/AppRuntimeTestSupport.h"

#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/async/Executor.h>
#include <ao/async/Sleeper.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Subscription.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/library/LibraryPaths.h>

#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

namespace ao::rt::test
{
  namespace
  {
    struct ReadyAudioBackend final : audio::NullBackend
    {
      audio::BackendId backendIdValue;
      audio::ProfileId profileIdValue;

      ReadyAudioBackend(audio::BackendId backendId, audio::ProfileId profileId)
        : backendIdValue{std::move(backendId)}, profileIdValue{std::move(profileId)}
      {
      }

      audio::BackendId backendId() const override { return backendIdValue; }
      audio::ProfileId profileId() const override { return profileIdValue; }
    };

    struct ReadyAudioProvider final : audio::BackendProvider
    {
      Status providerStatus;

      explicit ReadyAudioProvider(Status status)
        : providerStatus{std::move(status)}
      {
      }

      ReadyAudioProvider()
        : providerStatus{makeReadyAudioStatus()}
      {
      }

      void shutdown() noexcept override {}

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        if (callback)
        {
          callback(providerStatus.devices);
        }

        return {};
      }

      Status status() const override { return providerStatus; }

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& device,
                                                    audio::ProfileId const& profile) override
      {
        return std::make_unique<ReadyAudioBackend>(device.backendId, profile);
      }

      audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }
    };
  } // namespace

  audio::BackendProvider::Status makeReadyAudioStatus()
  {
    return {.descriptor =
              {
                .id = audio::BackendId{"test_backend"},
                .supportedProfiles =
                  {
                    {.id = audio::kProfileShared},
                  },
              },
            .devices = {
              audio::Device{.id = audio::DeviceId{"test_device"},
                            .displayName = "Test Device",
                            .description = "Ready test output",
                            .isDefault = true,
                            .backendId = audio::BackendId{"test_backend"}},
            }};
  }

  audio::BackendProvider::Status makePipeWireOutputStatus()
  {
    return {
      .descriptor =
        {
          .id = audio::BackendId{"pipewire"},
          .supportedProfiles =
            {
              {.id = audio::kProfileShared},
              {.id = audio::kProfileExclusive},
            },
        },
      .devices =
        {
          {
            .id = audio::DeviceId{"device1"},
            .displayName = "Built-in Audio",
            .description = "Built-in analog stereo",
            .isDefault = true,
            .backendId = audio::BackendId{"pipewire"},
          },
        },
    };
  }

  std::unique_ptr<audio::BackendProvider> makeReadyAudioProvider()
  {
    return std::make_unique<ReadyAudioProvider>();
  }

  std::unique_ptr<audio::BackendProvider> makeReadyAudioProvider(audio::BackendProvider::Status status)
  {
    return std::make_unique<ReadyAudioProvider>(std::move(status));
  }

  void addReadyAudioProvider(PlaybackTransport& transport)
  {
    PlaybackBootstrap{transport}.addProvider(makeReadyAudioProvider());
  }

  void addReadyAudioProvider(PlaybackTransport& transport, audio::BackendProvider::Status status)
  {
    PlaybackBootstrap{transport}.addProvider(makeReadyAudioProvider(std::move(status)));
  }

  void addReadyAudioProvider(AppRuntime& runtime)
  {
    runtime.addAudioProvider(makeReadyAudioProvider());
  }

  void addReadyAudioProvider(AppRuntime& runtime, audio::BackendProvider::Status status)
  {
    runtime.addAudioProvider(makeReadyAudioProvider(std::move(status)));
  }

  std::unique_ptr<AppRuntime> makeRuntime(ao::test::TempDir const& tempDir,
                                          std::unique_ptr<async::Executor> executorPtr,
                                          ConfigStore* const playbackSessionConfigStore,
                                          async::Sleeper* const sleeper)
  {
    return ao::test::requireValue(AppRuntime::create(AppRuntimeDependencies{
      .executorPtr = std::move(executorPtr),
      .musicRoot = tempDir.path(),
      .databasePath = LibraryPaths{tempDir.path()}.databasePath(),
      .musicLibraryMapSize = library::test::kTestMusicLibraryMapSize,
      .workspaceConfigStorePtr =
        std::make_unique<ConfigStore>(std::filesystem::path{tempDir.path()} / "workspace.yaml"),
      .playbackSessionConfigStore = playbackSessionConfigStore,
      .sleeper = sleeper,
    }));
  }

  std::unique_ptr<AppRuntime> makeRuntime(ao::test::TempDir const& tempDir,
                                          ConfigStore* const playbackSessionConfigStore,
                                          async::Sleeper* const sleeper)
  {
    return makeRuntime(tempDir, std::make_unique<InlineExecutor>(), playbackSessionConfigStore, sleeper);
  }
} // namespace ao::rt::test
