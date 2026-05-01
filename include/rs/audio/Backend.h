// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/Type.h>
#include <rs/audio/Format.h>
#include <rs/audio/flow/Graph.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rs::audio
{
  enum class BackendKind
  {
    None,
    PipeWire,          // shared/mixing mode
    PipeWireExclusive, // exclusive/direct hardware
    AlsaExclusive,
  };

  /**
   * @brief Helper to get the user-friendly name for a backend kind.
   */
  inline std::string_view backendDisplayName(BackendKind kind)
  {
    switch (kind)
    {
      case BackendKind::PipeWire: return "PipeWire (Shared)";
      case BackendKind::PipeWireExclusive: return "PipeWire (Exclusive)";
      case BackendKind::AlsaExclusive: return "ALSA (Exclusive)";
      default: return "Unknown";
    }
  }

  /**
   * @brief Helper to get the shorthand name for a backend kind.
   */
  inline std::string_view backendShortName(BackendKind kind)
  {
    switch (kind)
    {
      case BackendKind::PipeWire: return "PipeWire(S)";
      case BackendKind::PipeWireExclusive: return "PipeWire(E)";
      case BackendKind::AlsaExclusive: return "ALSA(E)";
      default: return "Output";
    }
  }

  /**
   * @brief Helper to get the internal ID string for a backend kind (used for actions/serialization).
   */
  inline std::string_view backendKindToId(BackendKind kind)
  {
    switch (kind)
    {
      case BackendKind::PipeWire: return "pipewire";
      case BackendKind::PipeWireExclusive: return "pipewire_exclusive";
      case BackendKind::AlsaExclusive: return "alsa";
      default: return "none";
    }
  }

  /**
   * @brief Helper to get the backend kind from an internal ID string.
   */
  inline BackendKind backendKindFromId(std::string_view id)
  {
    if (id == "pipewire") return BackendKind::PipeWire;
    if (id == "pipewire_exclusive") return BackendKind::PipeWireExclusive;
    if (id == "alsa") return BackendKind::AlsaExclusive;
    return BackendKind::None;
  }

  /**
   * @brief Hardware capabilities of an audio device.
   */
  struct DeviceCapabilities final
  {
    std::vector<std::uint32_t> sampleRates;
    std::vector<std::uint8_t> bitDepths;
    std::vector<std::uint8_t> channelCounts;

    bool operator==(DeviceCapabilities const&) const = default;
  };

  /**
   * @brief Describes an available audio output device.
   */
  struct Device final
  {
    std::string id;
    std::string displayName;
    std::string description;
    bool isDefault = false;
    BackendKind backendKind = BackendKind::None;
    DeviceCapabilities capabilities = {};

    bool operator==(Device const&) const = default;
  };

  /**
   * @brief Final conclusion on the quality of the current audio path.
   */
  enum class Quality
  {
    Unknown,
    BitwisePerfect,
    LosslessPadded,
    LosslessFloat,
    LinearIntervention,
    LossySource,
    Clipped,
  };
} // namespace rs::audio
