// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Format.h>
#include <ao/audio/Types.h>
#include <ao/audio/backend/detail/AudioBackendShared.h>
#include <ao/audio/flow/Graph.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern "C"
{
  struct spa_dict;
  struct spa_pod;
}

namespace ao::audio::backend::detail
{
  struct NodeRecord final
  {
    std::uint32_t version = 0;
    std::string mediaClass;
    std::string nodeName;
    std::string nodeNick;
    std::string nodeDescription;
    std::string objectPath;
    std::optional<std::uint32_t> objectSerial;
    std::optional<std::uint32_t> driverId;
  };

  struct LinkRecord final
  {
    std::uint32_t outputNodeId = 0xffffffffu; // PW_ID_ANY
    std::uint32_t inputNodeId = 0xffffffffu;
    int state = 0; // pw_link_state
  };

  struct SinkProps final
  {
    float volume = 1.0F;
    bool isMuted = false;
    std::vector<float> channelVolumes;
    bool isSoftMuted = false;
    std::vector<float> softVolumes;

    /**
     * @brief Checks if all volume components are at unity (1.0).
     */
    bool isUnity() const noexcept;
  };

  /**
   * @brief Checks if a media class string represents an audio sink.
   */
  bool isSinkMediaClass(std::string_view mediaClass) noexcept;

  /**
   * @brief Helper to lookup a property in a SPA dict.
   */
  std::string lookupProperty(::spa_dict const* props, char const* key) noexcept;

  /**
   * @brief Parses a NodeRecord from PipeWire registry events.
   */
  NodeRecord parseNodeRecord(std::uint32_t version, ::spa_dict const* props) noexcept;

  /**
   * @brief Converts a SPA audio format ID to Aobus SampleFormatCapability.
   */
  std::optional<SampleFormatCapability> sampleFormatCapabilityFromSpaFormat(std::uint32_t spaFmt) noexcept;

  /**
   * @brief Parses an EnumFormat SPA pod into DeviceCapabilities.
   */
  void parseEnumFormat(::spa_pod const* param, DeviceCapabilities& caps);

  /**
   * @brief Merges sink properties from a SPA Props pod.
   */
  void mergeSinkProps(SinkProps& sinkProps, ::spa_pod const* param);
} // namespace ao::audio::backend::detail
