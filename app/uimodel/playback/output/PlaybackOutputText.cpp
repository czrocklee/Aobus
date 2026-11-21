// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/playback/output/PlaybackOutputText.h>

#include <ao/audio/BackendIds.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/PlaybackState.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  using i18n::MessageCatalog;
  using i18n::MessageId;
  using i18n::requiredFormat;
  using i18n::requiredText;

  AudioBackendPresentation audioBackendPresentation(MessageCatalog const& catalog, audio::BackendId const& id)
  {
    if (id == audio::kBackendPipeWire)
    {
      return AudioBackendPresentation{
        .label = "PipeWire",
        .description = std::string{requiredText(catalog, MessageId::AudioBackendPipeWireDescription)},
        .shortLabel = "PW",
        .outputDeviceDescriptionFallback = "PipeWire",
        .iconKind = AudioIconKind::AudioServer,
      };
    }

    if (id == audio::kBackendAlsa)
    {
      return AudioBackendPresentation{
        .label = "ALSA",
        .description = std::string{requiredText(catalog, MessageId::AudioBackendAlsaDescription)},
        .shortLabel = "ALSA",
        .iconKind = AudioIconKind::OutputDevice,
      };
    }

    if (id == audio::kBackendWasapi)
    {
      return AudioBackendPresentation{
        .label = "WASAPI",
        .description = std::string{requiredText(catalog, MessageId::AudioBackendWasapiDescription)},
        .shortLabel = "WASAPI",
        .outputDeviceDescriptionFallback =
          std::string{requiredText(catalog, MessageId::AudioBackendWasapiOutputFallback)},
        .iconKind = AudioIconKind::OutputDevice,
      };
    }

    if (id == audio::kBackendCoreAudio)
    {
      return AudioBackendPresentation{
        .label = "Core Audio",
        .description = std::string{requiredText(catalog, MessageId::AudioBackendCoreAudioDescription)},
        .shortLabel = "Core Audio",
        .outputDeviceDescriptionFallback =
          std::string{requiredText(catalog, MessageId::AudioBackendCoreAudioOutputFallback)},
        .iconKind = AudioIconKind::OutputDevice,
      };
    }

    auto const& fallback = id.raw();
    return AudioBackendPresentation{.label = std::string{fallback}, .shortLabel = std::string{fallback}};
  }

  AudioProfilePresentation audioProfilePresentation(MessageCatalog const& catalog, audio::ProfileId const& id)
  {
    if (id == audio::kProfileShared)
    {
      return AudioProfilePresentation{
        .label = std::string{requiredText(catalog, MessageId::AudioProfileShared)},
        .description = std::string{requiredText(catalog, MessageId::AudioProfileSharedDescription)},
      };
    }

    if (id == audio::kProfileExclusive)
    {
      return AudioProfilePresentation{
        .label = std::string{requiredText(catalog, MessageId::AudioProfileExclusive)},
        .description = std::string{requiredText(catalog, MessageId::AudioProfileExclusiveDescription)},
      };
    }

    return AudioProfilePresentation{.label = std::string{id.raw()}};
  }

  std::optional<std::string> outputDeviceName(MessageCatalog const& catalog,
                                              rt::OutputState const& output,
                                              audio::OutputDeviceSelection const& selection)
  {
    if (selection.backendId == audio::kBackendNone)
    {
      return std::nullopt;
    }

    auto const backend =
      std::ranges::find(output.availableBackends, selection.backendId, &rt::OutputBackendSnapshot::id);

    if (backend == output.availableBackends.end())
    {
      return std::nullopt;
    }

    auto const device = std::ranges::find(backend->devices, selection.deviceId, &rt::OutputDeviceSnapshot::id);

    if (device == backend->devices.end())
    {
      return std::nullopt;
    }

    if (device->isDefault && device->id.empty() && device->displayName.empty())
    {
      return std::string{requiredText(catalog, MessageId::SystemDefaultOutputDevice)};
    }

    return device->displayName;
  }

  std::optional<std::string> selectedOutputDeviceName(MessageCatalog const& catalog, rt::OutputState const& output)
  {
    if (output.optPendingOutputSelection ||
        output.selectedDevice.routingPolicy == audio::OutputRoutingPolicy::AllowSystemReroute)
    {
      return std::nullopt;
    }

    return outputDeviceName(catalog, output, output.selectedDevice);
  }

  std::string volumeTooltip(MessageCatalog const& catalog,
                            std::int32_t const percent,
                            bool const muted,
                            bool const hardwareAssisted)
  {
    auto state = std::string_view{"other"};

    if (muted)
    {
      state = "muted";
    }
    else if (hardwareAssisted)
    {
      state = "hardware";
    }

    return requiredFormat(catalog, MessageId::PlaybackVolumeTooltip, {{"percent", percent}, {"state", state}});
  }
} // namespace ao::uimodel
