// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
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

      for (auto const& backend : state.availableOutputBackends)
      {
        if (backend.id != state.selectedOutputDevice.backendId)
        {
          continue;
        }

        result.backend = backend.name;

        for (auto const& device : backend.devices)
        {
          if (device.id == state.selectedOutputDevice.deviceId)
          {
            result.device = device.displayName;
            break;
          }
        }

        break;
      }

      if (result.backend.empty())
      {
        result.backend = state.selectedOutputDevice.backendId.raw();
      }

      if (result.device.empty())
      {
        result.device = state.selectedOutputDevice.deviceId.raw();
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

    for (auto const& backend : state.availableOutputBackends)
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
        for (auto const& profileMeta : backend.supportedProfiles)
        {
          auto const profile = profileMeta.id;
          bool const isExclusive = (profile == audio::kProfileExclusive);

          bool const isActive =
            (backend.id == state.selectedOutputDevice.backendId && profile == state.selectedOutputDevice.profileId &&
             device.id == state.selectedOutputDevice.deviceId);

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

    if (state.selectedOutputDevice.backendId == audio::kBackendNone)
    {
      view.outputBackendSummary = "--";
    }
    else
    {
      view.hasActiveOutputDevice = true;

      if (state.selectedOutputDevice.backendId == audio::kBackendPipeWire)
      {
        view.outputBackendSummary = "PW";
      }
      else if (state.selectedOutputDevice.backendId == audio::kBackendAlsa)
      {
        view.outputBackendSummary = "ALSA";
      }
      else
      {
        for (auto const& backend : state.availableOutputBackends)
        {
          if (backend.id == state.selectedOutputDevice.backendId)
          {
            view.outputBackendSummary = backend.name;
            break;
          }
        }

        if (view.outputBackendSummary.empty())
        {
          view.outputBackendSummary = state.selectedOutputDevice.backendId.raw();
        }
      }

      auto const names = resolveBackendDeviceNames(state);
      bool const isExclusive = (state.selectedOutputDevice.profileId == audio::kProfileExclusive);

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
