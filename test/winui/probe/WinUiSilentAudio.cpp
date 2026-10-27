// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "WinUiSilentAudio.h"

#include "lib/audio/NullBackend.h"
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/utility/ScopedRegistration.h>

#include <memory>
#include <string_view>
#include <vector>

namespace ao::winui::test
{
  namespace
  {
    class SilentBackend final : public audio::NullBackend
    {
    public:
      audio::BackendId backendId() const override { return silentAudioDevice().backendId; }
      audio::ProfileId profileId() const override { return audio::ProfileId{audio::kProfileShared}; }
    };

    class SilentProvider final : public audio::BackendProvider
    {
    public:
      void shutdown() noexcept override { _closed = true; }

      Status status() const override
      {
        return {.descriptor = {.id = silentAudioDevice().backendId,
                               .supportedProfiles = {{.id = audio::ProfileId{audio::kProfileShared}}}},
                .devices = _closed ? std::vector<audio::Device>{} : std::vector{silentAudioDevice()}};
      }

      utility::ScopedRegistration subscribeDevices(OnDevicesChangedCallback callback) override
      {
        if (!_closed)
        {
          callback(status().devices);
        }

        return {};
      }

      std::unique_ptr<audio::Backend> createBackend(audio::Device const& /*device*/,
                                                    audio::ProfileId const& /*profile*/) override
      {
        return std::make_unique<SilentBackend>();
      }

      utility::ScopedRegistration subscribeGraph(std::string_view /*routeAnchor*/,
                                                 OnGraphChangedCallback /*callback*/) override
      {
        return {};
      }

    private:
      bool _closed = false;
    };
  } // namespace

  audio::Device silentAudioDevice()
  {
    return {.id = audio::DeviceId{"silent"},
            .displayName = "Silent transport probe",
            .isDefault = true,
            .backendId = audio::BackendId{"winui-transport-probe"}};
  }

  void addSilentAudioOutput(rt::AppRuntime& runtime)
  {
    runtime.addAudioProvider(std::make_unique<SilentProvider>());
    auto const device = silentAudioDevice();
    runtime.playback().commands().setOutputDevice(device.backendId, device.id, audio::ProfileId{audio::kProfileShared});
  }
} // namespace ao::winui::test
