// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/uimodel/playback/AudioOutputViewModel.h>

#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::playback
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

      for (auto const& backend : state.availableOutputs)
      {
        if (backend.id != state.selectedOutput.backendId)
        {
          continue;
        }

        result.backend = backend.name;

        for (auto const& device : backend.devices)
        {
          if (device.id == state.selectedOutput.deviceId)
          {
            result.device = device.displayName;
            break;
          }
        }

        break;
      }

      if (result.backend.empty())
      {
        result.backend = state.selectedOutput.backendId.raw();
      }

      if (result.device.empty())
      {
        result.device = state.selectedOutput.deviceId.raw();
      }

      return result;
    }
  } // namespace

  AudioOutputViewModel::AudioOutputViewModel(rt::PlaybackService& playback,
                                             std::function<void(AudioOutputViewState const&)> onRender)
    : _playback{playback}, _onRender{std::move(onRender)}
  {
    _outputChangedSub = _playback.onOutputChanged([this](auto const&) { refresh(); });
  }

  void AudioOutputViewModel::selectOutput(audio::BackendId const& backendId,
                                          audio::DeviceId const& deviceId,
                                          audio::ProfileId const& profileId)
  {
    _playback.setOutput(backendId, deviceId, profileId);
  }

  void AudioOutputViewModel::refresh()
  {
    auto const& state = _playback.state();
    auto view = AudioOutputViewState{};

    for (auto const& backend : state.availableOutputs)
    {
      view.rows.push_back(AudioOutputRow{
        .kind = AudioOutputRow::Kind::BackendHeader,
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
            (backend.id == state.selectedOutput.backendId && profile == state.selectedOutput.profileId &&
             device.id == state.selectedOutput.deviceId);

          view.rows.push_back(AudioOutputRow{
            .kind = AudioOutputRow::Kind::DeviceProfile,
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

    if (state.selectedOutput.backendId == audio::kBackendNone)
    {
      view.backendSummary = "--";
    }
    else
    {
      view.hasActiveOutput = true;

      if (state.selectedOutput.backendId == audio::kBackendPipeWire)
      {
        view.backendSummary = "PW";
      }
      else if (state.selectedOutput.backendId == audio::kBackendAlsa)
      {
        view.backendSummary = "ALSA";
      }
      else
      {
        for (auto const& backend : state.availableOutputs)
        {
          if (backend.id == state.selectedOutput.backendId)
          {
            view.backendSummary = backend.name;
            break;
          }
        }

        if (view.backendSummary.empty())
        {
          view.backendSummary = state.selectedOutput.backendId.raw();
        }
      }

      auto const names = resolveBackendDeviceNames(state);
      bool const isExclusive = (state.selectedOutput.profileId == audio::kProfileExclusive);

      view.outputStatus = std::format("{}: {}", names.backend, names.device);

      if (isExclusive)
      {
        view.outputStatus += " (Exclusive Mode)";
      }
    }

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel::playback
