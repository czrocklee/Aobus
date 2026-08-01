// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackTransportTestSupport.h"

#include "lib/audio/detail/DecoderOutput.h"
#include "runtime/playback/PlaybackBootstrap.h"
#include "runtime/playback/PlaybackTransport.h"
#include "test/unit/audio/BackendTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/OpenedPcmMode.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Player.h>
#include <ao/audio/Property.h>
#include <ao/audio/RenderTarget.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/Subscription.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/ViewIds.h>

#include <catch2/catch_test_macros.hpp>
#include <fakeit.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::rt::test
{
  PlaybackTransport::PlaybackRequest playbackRequest(TrackId const trackId,
                                                     std::string_view const filePath,
                                                     std::string title,
                                                     std::string artist,
                                                     std::chrono::milliseconds const duration,
                                                     std::string album,
                                                     ResourceId const coverArtId,
                                                     ViewId const sourceViewId)
  {
    return PlaybackTransport::PlaybackRequest{
      .item = NowPlayingInfo{.trackId = trackId,
                             .sourceViewId = sourceViewId,
                             .coverArtId = coverArtId,
                             .title = std::move(title),
                             .artist = std::move(artist),
                             .album = std::move(album)},
      .input = audio::PlaybackInput{.filePath = std::string{filePath}, .duration = duration},
    };
  }

  audio::BackendProvider::Status makeMockProviderStatus()
  {
    auto status = audio::BackendProvider::Status{};
    status.descriptor.id = audio::BackendId{"mock_backend"};
    status.devices.push_back(audio::Device{.id = audio::DeviceId{"mock_device"},
                                           .displayName = "Mock Device",
                                           .description = "A mock audio device",
                                           .isDefault = true,
                                           .backendId = audio::BackendId{"mock_backend"}});
    status.descriptor.supportedProfiles.push_back(
      audio::BackendProvider::ProfileDescriptor{.id = audio::ProfileId{audio::kProfileShared}});
    return status;
  }

  namespace detail
  {
    std::shared_ptr<PlaybackFixtureSpyBackend> makePlaybackFixtureSpyBackend()
    {
      return std::make_shared<PlaybackFixtureSpyBackend>();
    }

    std::unique_ptr<audio::Player> makePlaybackFixturePlayer(async::Runtime& runtime,
                                                             audio::DecoderFactoryFn decoderFactory)
    {
      if (decoderFactory)
      {
        return std::make_unique<audio::Player>(runtime, std::move(decoderFactory));
      }

      return std::make_unique<audio::Player>(runtime);
    }

    void configurePlaybackTransportFixture(std::shared_ptr<PlaybackFixtureSpyBackend>& spyBackendPtr,
                                           fakeit::Mock<audio::BackendProvider>& mockProvider,
                                           audio::BackendProvider::Status& status,
                                           audio::BackendProvider::OnDevicesChangedCallback& onDevicesChangedCb,
                                           audio::BackendProvider::OnGraphChangedCallback& onGraphChangedCb,
                                           audio::RenderTarget*& renderTarget,
                                           PlaybackTransport& playbackTransport)
    {
      auto* const onDevicesChangedCbAddress = &onDevicesChangedCb;
      auto* const onGraphChangedCbAddress = &onGraphChangedCb;
      auto* const renderTargetAddress = &renderTarget;
      auto* const spyBackendOwnerAddress = &spyBackendPtr;

      fakeit::Fake(Method(mockProvider, shutdown));

      fakeit::When(Method(mockProvider, subscribeDevices))
        .AlwaysDo(
          [onDevicesChangedCbAddress](audio::BackendProvider::OnDevicesChangedCallback cb)
          {
            *onDevicesChangedCbAddress = std::move(cb);
            return audio::Subscription{};
          });

      fakeit::When(Method(mockProvider, subscribeGraph))
        .AlwaysDo(
          [onGraphChangedCbAddress](std::string_view, audio::BackendProvider::OnGraphChangedCallback cb)
          {
            *onGraphChangedCbAddress = std::move(cb);
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
          [renderTargetAddress](
            audio::SignalFormat const& sourceFormat, audio::RenderTarget*& target) -> Result<audio::OpenedPcmMode>
          {
            *renderTargetAddress = target;
            auto const encodings = audio::detail::losslessPcmEncodings(sourceFormat);
            REQUIRE_FALSE(encodings.empty());
            return audio::OpenedPcmMode{.clientFormat = audio::pcmFormat(sourceFormat, encodings.front())};
          });
      fakeit::When(Method(mockProvider, createBackend))
        .AlwaysDo([spyBackendOwnerAddress](audio::Device const&, audio::ProfileId const&)
                  { return (*spyBackendOwnerAddress)->makeProxy(); });

      PlaybackBootstrap{playbackTransport}.addProvider(
        std::make_unique<audio::test::MockProviderProxy>(mockProvider.get()));
    }
  } // namespace detail
} // namespace ao::rt::test
