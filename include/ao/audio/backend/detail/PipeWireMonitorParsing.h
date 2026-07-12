// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Device.h>
#include <ao/audio/Format.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

extern "C"
{
#include <spa/pod/pod.h>
#include <spa/utils/dict.h>
}

namespace ao::audio::backend::detail
{
  static constexpr std::uint32_t kPwIdAny = 0xFFFFFFFFU;

  struct NodeRecord final
  {
    std::uint32_t version = 0;
    std::string mediaClass;
    std::string nodeName;
    std::string nodeNick;
    std::string nodeDescription;
    std::string objectPath;
    std::optional<std::uint32_t> optObjectSerial;
    std::optional<std::uint32_t> optDriverId;
  };

  struct LinkRecord final
  {
    std::uint32_t outputNodeId = kPwIdAny; // PW_ID_ANY
    std::uint32_t inputNodeId = kPwIdAny;
    std::int32_t state = 0; // pw_link_state
  };

  struct SinkProps final
  {
    enum class VolumeClassificationContext : std::uint8_t
    {
      Sink,
      Stream,
    };

    bool hasVolume = false;
    bool volumeIsHardware = false;
    float volume = 1.0F;

    bool isMuted = false;

    bool hasChannelVolumes = false;
    bool channelVolumesAreHardware = false;
    std::vector<float> channelVolumes;

    bool isSoftMuted = false;
    bool hasSoftVolumes = false;
    std::vector<float> softVolumes;

    struct VolumeClassification final
    {
      bool hardwareNotUnity = false;
      bool softwareNotUnity = false;
      float maxSoftwareGain = 0.0F;
      float minSoftwareGain = 0.0F;
      bool unclassifiedNotUnity = false;
    };

    /**
     * @brief Classifies the volume state based on hardware and software evidence.
     */
    VolumeClassification classifyVolume(
      VolumeClassificationContext context = VolumeClassificationContext::Sink) const noexcept;
  };

  /**
   * @brief Checks if a media class string represents an audio sink.
   */
  bool isSinkMediaClass(std::string_view mediaClass) noexcept;

  /**
   * @brief Looks up a property in a SPA dict.
   */
  std::string lookupProperty(::spa_dict const* props, char const* key) noexcept;

  /**
   * @brief Parses a PipeWire decimal uint32 string.
   */
  std::optional<std::uint32_t> parsePipeWireUint32(char const* value) noexcept;

  /**
   * @brief Parses a NodeRecord from PipeWire registry events.
   */
  NodeRecord parseNodeRecord(std::uint32_t version, ::spa_dict const* props) noexcept;

  /**
   * @brief Converts a SPA audio format ID to Aobus SampleFormatCapability.
   */
  std::optional<SampleFormatCapability> sampleFormatCapabilityFromSpaFormat(std::uint32_t spaFmt) noexcept;

  /**
   * @brief Parses an EnumFormat SPA pod into DeviceFormatCapabilities.
   */
  void parseEnumFormat(::spa_pod const* param, DeviceFormatCapabilities& caps);

  /**
   * @brief Returns the current node format only for active Format params.
   *
   * EnumFormat params describe supported candidates and must not be treated as
   * the current stream/sink format.
   */
  std::optional<Format> currentFormatFromNodeParam(std::uint32_t paramId, ::spa_pod const* param) noexcept;

  /**
   * @brief Updates the live format cache from an active Format param.
   *
   * A failed/null current Format invalidates any previously cached format for
   * the node; EnumFormat and other params are capabilities or unrelated state
   * and do not touch the live-format cache.
   */
  void updateCurrentFormatFromNodeParam(std::unordered_map<std::uint32_t, Format>& nodeFormatMap,
                                        std::uint32_t nodeId,
                                        std::uint32_t paramId,
                                        ::spa_pod const* param);

  /**
   * @brief Merges sink properties from a SPA Props pod.
   */
  void mergeSinkProps(SinkProps& sinkProps, ::spa_pod const* param);
} // namespace ao::audio::backend::detail
