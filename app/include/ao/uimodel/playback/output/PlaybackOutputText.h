// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendIds.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ao::audio
{
  struct OutputDeviceSelection;
}

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  struct OutputState;
}

namespace ao::uimodel
{
  enum class AudioIconKind : std::uint8_t
  {
    OutputDevice,
    AudioServer,
  };

  struct AudioBackendPresentation final
  {
    std::string label{};
    std::string description{};
    std::string shortLabel{};
    std::string outputDeviceDescriptionFallback{};
    AudioIconKind iconKind = AudioIconKind::OutputDevice;

    bool operator==(AudioBackendPresentation const&) const = default;
  };

  struct AudioProfilePresentation final
  {
    std::string label{};
    std::string description{};

    bool operator==(AudioProfilePresentation const&) const = default;
  };

  AudioBackendPresentation audioBackendPresentation(i18n::MessageCatalog const& catalog, audio::BackendId const& id);
  AudioProfilePresentation audioProfilePresentation(i18n::MessageCatalog const& catalog, audio::ProfileId const& id);

  /// A disabled or unlisted route returns nullopt; a matched route may still have an empty name.
  std::optional<std::string> outputDeviceName(i18n::MessageCatalog const& catalog,
                                              rt::OutputState const& output,
                                              audio::OutputDeviceSelection const& selection);

  /// Suppresses the name when a request is pending or the system may use another output.
  std::optional<std::string> selectedOutputDeviceName(i18n::MessageCatalog const& catalog,
                                                      rt::OutputState const& output);

  std::string volumeTooltip(i18n::MessageCatalog const& catalog,
                            std::int32_t percent,
                            bool muted,
                            bool hardwareAssisted);
} // namespace ao::uimodel
