// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/AudioOutputViewModel.h>

#include <format>
#include <functional>
#include <utility>
#include <vector>

namespace ao::uimodel::playback
{
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
          auto const displayName =
            (profile == audio::kProfileExclusive) ? std::format("{} [E]", device.displayName) : device.displayName;

          bool const isActive =
            (backend.id == state.selectedOutput.backendId && profile == state.selectedOutput.profileId &&
             device.id == state.selectedOutput.deviceId);

          view.rows.push_back(AudioOutputRow{
            .kind = AudioOutputRow::Kind::DeviceProfile,
            .backendId = backend.id,
            .deviceId = device.id,
            .profileId = profile,
            .title = displayName,
            .description = device.description,
            .isActive = isActive,
          });
        }
      }
    }

    if (_onRender)
    {
      _onRender(view);
    }
  }
} // namespace ao::uimodel::playback
