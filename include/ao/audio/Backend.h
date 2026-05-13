// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/audio/Format.h>
#include <ao/audio/flow/Graph.h>

#include <ao/utility/StrongId.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio
{
  /**
   * @brief Type aliases for backend and profile identification.
   */
  using BackendId = ao::utility::StrongId<struct BackendTag>;
  using ProfileId = ao::utility::StrongId<struct ProfileTag>;
  using DeviceId = ao::utility::StrongId<struct DeviceTag>;

  /**
   * @brief Standard Backend and Profile IDs.
   */
  inline BackendId const kBackendNone{""};
  inline BackendId const kBackendPipeWire{"pipewire"};
  inline BackendId const kBackendAlsa{"alsa"};

  inline ProfileId const kProfileShared{"shared"};
  inline ProfileId const kProfileExclusive{"exclusive"};

  /**
   * @brief A concrete PCM sample format supported by a device.
   */
  struct SampleFormatCapability final
  {
    std::uint8_t bitDepth = 0;
    std::uint8_t validBits = 0;
    bool isFloat = false;

    bool operator==(SampleFormatCapability const&) const = default;
  };

  /**
   * @brief Hardware capabilities of an audio device.
   */
  struct DeviceCapabilities final
  {
    std::vector<std::uint32_t> sampleRates;
    std::vector<SampleFormatCapability> sampleFormats;
    std::vector<std::uint8_t> bitDepths;
    std::vector<std::uint8_t> channelCounts;

    bool operator==(DeviceCapabilities const&) const = default;
  };

  /**
   * @brief Describes an available audio output device.
   */
  struct Device final
  {
    DeviceId id;
    std::string displayName;
    std::string description;
    bool isDefault = false;
    BackendId backendId;
    DeviceCapabilities capabilities = {};

    bool operator==(Device const&) const = default;
  };

  /**
   * @brief Identifies a specific node in a backend's internal graph.
   */
  struct RouteAnchor final
  {
    BackendId backend;
    std::string id;

    bool operator==(RouteAnchor const&) const = default;
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
} // namespace ao::audio
