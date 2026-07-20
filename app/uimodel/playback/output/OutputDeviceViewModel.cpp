// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
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

    BackendDeviceNames resolveBackendDeviceNames(rt::OutputState const& output,
                                                 PresentationTextCatalog const& textCatalog)
    {
      auto result = BackendDeviceNames{};

      for (auto const& backend : output.availableBackends)
      {
        if (backend.id != output.selectedDevice.backendId)
        {
          continue;
        }

        result.backend = textCatalog.audioBackend(backend.id).label;

        for (auto const& device : backend.devices)
        {
          if (device.id == output.selectedDevice.deviceId)
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
        result.backend = output.selectedDevice.backendId.raw();
      }

      if (result.device.empty())
      {
        result.device = output.selectedDevice.deviceId.raw();
      }

      return result;
    }
  } // namespace

  OutputDeviceViewModel::OutputDeviceViewModel(rt::PlaybackService& playback,
                                               std::function<void(OutputDeviceViewState const&)> onRender)
    : _playback{playback}
    , _commands{playback.commands()}
    , _onRender{std::move(onRender)}
    , _lastOutput{playback.snapshot().transport.output}
  {
    _snapshotSub =
      _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { handleSnapshot(snapshot); });
  }

  void OutputDeviceViewModel::selectOutputDevice(audio::BackendId const& backendId,
                                                 audio::DeviceId const& deviceId,
                                                 audio::ProfileId const& profileId)
  {
    _commands.setOutputDevice(backendId, deviceId, profileId);
  }

  void OutputDeviceViewModel::refresh()
  {
    auto const& output = _playback.snapshot().transport.output;
    _lastOutput = output;
    render(output);
  }

  void OutputDeviceViewModel::handleSnapshot(rt::PlaybackSnapshot const& snapshot)
  {
    if (snapshot.transport.output == _lastOutput)
    {
      return;
    }

    _lastOutput = snapshot.transport.output;
    render(snapshot.transport.output);
  }

  void OutputDeviceViewModel::render(rt::OutputState const& output)
  {
    auto view = OutputDeviceViewState{};

    for (auto const& backend : output.availableBackends)
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
            (backend.id == output.selectedDevice.backendId && profile == output.selectedDevice.profileId &&
             device.id == output.selectedDevice.deviceId);

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

    if (output.selectedDevice.backendId == audio::kBackendNone)
    {
      view.outputBackendSummary = "--";
    }
    else
    {
      view.hasActiveOutputDevice = true;

      view.outputBackendSummary = _textCatalog.audioBackend(output.selectedDevice.backendId).shortLabel;

      auto const names = resolveBackendDeviceNames(output, _textCatalog);
      bool const isExclusive = (output.selectedDevice.profileId == audio::kProfileExclusive);

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
