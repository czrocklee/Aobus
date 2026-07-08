// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    struct BackendDeviceNames final
    {
      std::string backend;
      std::string device;
    };

    BackendDeviceNames resolveBackendDeviceNames(rt::PlaybackState const& state)
    {
      auto result = BackendDeviceNames{};

      for (auto const& backend : state.output.availableBackends)
      {
        if (backend.id != state.output.selectedDevice.backendId)
        {
          continue;
        }

        result.backend = backend.name;

        for (auto const& device : backend.devices)
        {
          if (device.id == state.output.selectedDevice.deviceId)
          {
            result.device = device.displayName;
            break;
          }
        }

        break;
      }

      if (result.backend.empty())
      {
        result.backend = state.output.selectedDevice.backendId.raw();
      }

      if (result.device.empty())
      {
        result.device = state.output.selectedDevice.deviceId.raw();
      }

      return result;
    }
  } // namespace

  OutputDeviceViewModel::OutputDeviceViewModel(rt::PlaybackService& playback,
                                               std::function<void(OutputDeviceViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    _outputDeviceChangedSub = _playback.onOutputDeviceChanged([this](auto const&) { refresh(); });
  }

  void OutputDeviceViewModel::selectOutputDevice(audio::BackendId const& backendId,
                                                 audio::DeviceId const& deviceId,
                                                 audio::ProfileId const& profileId)
  {
    _playback.setOutputDevice(backendId, deviceId, profileId);
  }

  void OutputDeviceViewModel::refresh()
  {
    auto const& state = _playback.state();
    auto view = OutputDeviceViewState{};

    for (auto const& backend : state.output.availableBackends)
    {
      view.rows.push_back(OutputDeviceRow{
        .kind = OutputDeviceRow::Kind::BackendHeader,
        .backendId = backend.id,
        .deviceId = audio::DeviceId{},
        .profileId = audio::kProfileShared,
        .title = backend.name,
        .description = std::string{},
        .isActive = false,
      });

      for (auto const& device : backend.devices)
      {
        for (auto const& supportedProfile : backend.supportedProfiles)
        {
          auto const profile = supportedProfile.id;

          if (!rt::supportsOutputProfile(device, profile))
          {
            continue;
          }

          bool const isExclusive = (profile == audio::kProfileExclusive);

          bool const isActive =
            (backend.id == state.output.selectedDevice.backendId && profile == state.output.selectedDevice.profileId &&
             device.id == state.output.selectedDevice.deviceId);

          view.rows.push_back(OutputDeviceRow{
            .kind = OutputDeviceRow::Kind::DeviceProfile,
            .backendId = backend.id,
            .deviceId = device.id,
            .profileId = profile,
            .title = device.displayName,
            .description = device.description,
            .isActive = isActive,
            .isExclusive = isExclusive,
          });
        }
      }
    }

    if (state.output.selectedDevice.backendId == audio::kBackendNone)
    {
      view.outputBackendSummary = "--";
    }
    else
    {
      view.hasActiveOutputDevice = true;

      if (state.output.selectedDevice.backendId == audio::kBackendPipeWire)
      {
        view.outputBackendSummary = "PW";
      }
      else if (state.output.selectedDevice.backendId == audio::kBackendAlsa)
      {
        view.outputBackendSummary = "ALSA";
      }
      else
      {
        for (auto const& backend : state.output.availableBackends)
        {
          if (backend.id == state.output.selectedDevice.backendId)
          {
            view.outputBackendSummary = backend.name;
            break;
          }
        }

        if (view.outputBackendSummary.empty())
        {
          view.outputBackendSummary = state.output.selectedDevice.backendId.raw();
        }
      }

      auto const names = resolveBackendDeviceNames(state);
      bool const isExclusive = (state.output.selectedDevice.profileId == audio::kProfileExclusive);

      view.outputDeviceStatus = std::format("{}: {}", names.backend, names.device);

      if (isExclusive)
      {
        view.outputDeviceStatus += " (Exclusive Mode)";
      }
    }

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
