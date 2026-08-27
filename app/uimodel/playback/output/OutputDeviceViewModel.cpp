// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/uimodel/presentation/PresentationText.h>

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

    BackendDeviceNames resolveBackendDeviceNames(rt::OutputState const& output, i18n::MessageCatalog const& textCatalog)
    {
      auto result = BackendDeviceNames{};

      for (auto const& backend : output.availableBackends)
      {
        if (backend.id != output.selectedDevice.backendId)
        {
          continue;
        }

        result.backend = audioBackendPresentation(textCatalog, backend.id).label;

        for (auto const& device : backend.devices)
        {
          if (device.id == output.selectedDevice.deviceId)
          {
            result.device = device.displayName;

            if (device.isDefault && device.id.empty() && result.device.empty())
            {
              result.device = i18n::requiredText(textCatalog, i18n::MessageId::SystemDefaultOutputDevice);
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
                                               i18n::MessageCatalog textCatalog,
                                               RenderCallback onRender,
                                               OutputDeviceIntent intent)
    : _playback{playback}
    , _commands{playback.commands()}
    , _onRender{std::move(onRender)}
    , _intent{std::move(intent)}
    , _textCatalog{std::move(textCatalog)}
    , _lastOutput{playback.snapshot().transport.output}
  {
    _snapshotSub =
      _playback.events().onSnapshot([this](rt::PlaybackSnapshot const& snapshot) { handleSnapshot(snapshot); });
  }

  void OutputDeviceViewModel::selectOutputDevice(audio::BackendId const& backendId,
                                                 audio::DeviceId const& deviceId,
                                                 audio::ProfileId const& profileId)
  {
    auto const selection = audio::OutputDeviceSelection{
      .backendId = backendId,
      .deviceId = deviceId,
      .profileId = profileId,
    };
    _commands.setOutputDevice(selection.backendId, selection.deviceId, selection.profileId);
    _intent.record(selection);
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
      auto const backendPresentation = audioBackendPresentation(_textCatalog, backend.id);
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
            deviceTitle = i18n::requiredText(_textCatalog, i18n::MessageId::SystemDefaultOutputDevice);
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

      view.outputBackendSummary = audioBackendPresentation(_textCatalog, output.selectedDevice.backendId).shortLabel;

      auto const names = resolveBackendDeviceNames(output, _textCatalog);
      bool const isExclusive = (output.selectedDevice.profileId == audio::kProfileExclusive);

      view.outputDeviceStatus = std::format("{}: {}", names.backend, names.device);

      if (isExclusive)
      {
        view.outputDeviceStatus += " (" + audioProfilePresentation(_textCatalog, audio::kProfileExclusive).label + ")";
      }
    }

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel
