// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/PlaybackState.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

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

    BackendDeviceNames resolveBackendDeviceNames(rt::PlaybackState const& state,
                                                 PresentationTextCatalog const& textCatalog)
    {
      auto result = BackendDeviceNames{};

      for (auto const& backend : state.output.availableBackends)
      {
        if (backend.id != state.output.selectedDevice.backendId)
        {
          continue;
        }

        result.backend = textCatalog.audioBackend(backend.id).label;

        for (auto const& device : backend.devices)
        {
          if (device.id == state.output.selectedDevice.deviceId)
          {
            result.device = device.displayName;

            if (device.isDefault && device.id.empty() && result.device.empty())
            {
              result.device = textCatalog.systemDefaultOutputDeviceLabel();
            }

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
      auto const backendPresentation = _textCatalog.audioBackend(backend.id);
      view.rows.push_back(OutputDeviceRow{
        .kind = OutputDeviceRow::Kind::BackendHeader,
        .backendId = backend.id,
        .deviceId = audio::DeviceId{},
        .profileId = audio::kProfileShared,
        .title = backendPresentation.label,
        .description = backendPresentation.description,
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
          auto deviceTitle = device.displayName;

          if (device.isDefault && device.id.empty() && deviceTitle.empty())
          {
            deviceTitle = _textCatalog.systemDefaultOutputDeviceLabel();
          }

          bool const isActive =
            (backend.id == state.output.selectedDevice.backendId && profile == state.output.selectedDevice.profileId &&
             device.id == state.output.selectedDevice.deviceId);

          view.rows.push_back(OutputDeviceRow{
            .kind = OutputDeviceRow::Kind::DeviceProfile,
            .backendId = backend.id,
            .deviceId = device.id,
            .profileId = profile,
            .title = std::move(deviceTitle),
            .description =
              device.description.empty() ? backendPresentation.outputDeviceDescriptionFallback : device.description,
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

      view.outputBackendSummary = _textCatalog.audioBackend(state.output.selectedDevice.backendId).shortLabel;

      auto const names = resolveBackendDeviceNames(state, _textCatalog);
      bool const isExclusive = (state.output.selectedDevice.profileId == audio::kProfileExclusive);

      view.outputDeviceStatus = std::format("{}: {}", names.backend, names.device);

      if (isExclusive)
      {
        view.outputDeviceStatus += " (" + _textCatalog.audioProfile(audio::kProfileExclusive).label + ")";
      }
    }

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
