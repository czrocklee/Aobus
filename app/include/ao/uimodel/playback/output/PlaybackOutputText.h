// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/BackendIds.h>

#include <cstdint>
#include <string>

namespace ao::i18n
{
  class MessageCatalog;
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
  std::string volumeTooltip(i18n::MessageCatalog const& catalog,
                            std::int32_t percent,
                            bool muted,
                            bool hardwareAssisted);
} // namespace ao::uimodel
