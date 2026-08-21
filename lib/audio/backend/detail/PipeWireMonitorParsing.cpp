// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "backend/detail/PipeWireMonitorParsing.h"

#include "backend/detail/PipeWireFormatParsing.h"
#include <ao/audio/PcmFormat.h>

extern "C"
{
#include <pipewire/keys.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/utils/dict.h>
#include <spa/utils/type.h>
}

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace ao::audio::backend::detail
{
  bool isSinkMediaClass(std::string_view mediaClass) noexcept
  {
    if (!mediaClass.contains("Audio"))
    {
      return false;
    }

    return mediaClass.ends_with("/Sink") || mediaClass.ends_with("/Duplex");
  }

  std::string lookupProperty(::spa_dict const* props, char const* key) noexcept
  {
    auto const* value = props != nullptr ? ::spa_dict_lookup(props, key) : nullptr;
    return value != nullptr ? std::string{value} : std::string{};
  }

  std::optional<std::uint32_t> parsePipeWireUint32(char const* value) noexcept
  {
    if (value == nullptr || *value == '\0')
    {
      return std::nullopt;
    }

    auto const text = std::string_view{value};
    std::uint32_t parsed = 0;
    auto const* const begin = text.data();
    auto const* const end = begin + text.size();
    auto const [ptr, ec] = std::from_chars(begin, end, parsed);

    if (ec != std::errc{} || ptr != end)
    {
      return std::nullopt;
    }

    return parsed;
  }

  NodeRecord parseNodeRecord(std::uint32_t version, ::spa_dict const* props) noexcept
  {
    auto record = NodeRecord{};
    record.version = version;
    record.mediaClass = lookupProperty(props, PW_KEY_MEDIA_CLASS);
    record.nodeName = lookupProperty(props, PW_KEY_NODE_NAME);
    record.nodeNick = lookupProperty(props, PW_KEY_NODE_NICK);
    record.nodeDescription = lookupProperty(props, PW_KEY_NODE_DESCRIPTION);
    record.objectPath = lookupProperty(props, PW_KEY_OBJECT_PATH);

    if (auto const optSerial = parsePipeWireUint32(::spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL)); optSerial)
    {
      record.optObjectSerial = optSerial;
    }

    if (auto const optId = parsePipeWireUint32(::spa_dict_lookup(props, "node.driver-id")); optId)
    {
      record.optDriverId = optId;
    }
    else if (auto const optIdFallback = parsePipeWireUint32(::spa_dict_lookup(props, "node.driver")); optIdFallback)
    {
      record.optDriverId = optIdFallback;
    }

    return record;
  }

  std::optional<PcmFormat> currentFormatFromNodeParam(std::uint32_t const paramId,
                                                      ::spa_pod const* const param) noexcept
  {
    if (paramId != SPA_PARAM_Format)
    {
      return std::nullopt;
    }

    return parseRawStreamFormat(param);
  }

  void updateCurrentFormatFromNodeParam(std::unordered_map<std::uint32_t, PcmFormat>& nodeFormatMap,
                                        std::uint32_t const nodeId,
                                        std::uint32_t const paramId,
                                        ::spa_pod const* const param)
  {
    if (paramId != SPA_PARAM_Format)
    {
      return;
    }

    if (auto const optFmt = currentFormatFromNodeParam(paramId, param); optFmt)
    {
      nodeFormatMap[nodeId] = *optFmt;
      return;
    }

    nodeFormatMap.erase(nodeId);
  }

  namespace
  {
    bool copyFloatValues(::spa_pod const& pod, std::vector<float>& output)
    {
      std::uint32_t count = 0;
      std::uint32_t valueSize = 0;
      std::uint32_t valueType = SPA_TYPE_None;
      auto const* values = static_cast<float const*>(::spa_pod_get_array_full(&pod, &count, &valueSize, &valueType));

      if (values == nullptr || count == 0U || valueSize != sizeof(float) || valueType != SPA_TYPE_Float)
      {
        return false;
      }

      output.assign(values, values + count);
      return true;
    }
  } // namespace

  void mergeSinkProps(SinkProps& sinkProps, ::spa_pod const* param)
  {
    if (param == nullptr)
    {
      return;
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_volume); prop != nullptr)
    {
      if (float val = 0.0F; ::spa_pod_get_float(&prop->value, &val) == 0)
      {
        sinkProps.hasVolume = true;

        if ((prop->flags & SPA_POD_PROP_FLAG_HARDWARE) != 0)
        {
          sinkProps.volumeIsHardware = true;
        }

        sinkProps.volume = val;
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_mute); prop != nullptr)
    {
      if (bool val = false; ::spa_pod_get_bool(&prop->value, &val) == 0)
      {
        sinkProps.isMuted = val;
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes); prop != nullptr)
    {
      if (copyFloatValues(prop->value, sinkProps.channelVolumes))
      {
        sinkProps.hasChannelVolumes = true;

        if ((prop->flags & SPA_POD_PROP_FLAG_HARDWARE) != 0)
        {
          sinkProps.channelVolumesAreHardware = true;
        }
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_softMute); prop != nullptr)
    {
      if (bool val = false; ::spa_pod_get_bool(&prop->value, &val) == 0)
      {
        sinkProps.isSoftMuted = val;
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_softVolumes); prop != nullptr)
    {
      if (copyFloatValues(prop->value, sinkProps.softVolumes))
      {
        sinkProps.hasSoftVolumes = true;
      }
    }
  }

  SinkProps::VolumeClassification SinkProps::classifyVolume(VolumeClassificationScope const scope) const noexcept
  {
    static constexpr float kUnityEpsilon = 1e-4F;
    auto const isNotUnity = [](float val) { return std::abs(val - 1.0F) >= kUnityEpsilon; };
    auto recordGain = [](float& maxGain, float& minGain, bool& initialized, float const gain)
    {
      if (!initialized)
      {
        maxGain = gain;
        minGain = gain;
        initialized = true;
        return;
      }

      maxGain = std::max(gain, maxGain);
      minGain = std::min(gain, minGain);
    };
    auto recordGains = [&recordGain](float& maxGain, float& minGain, bool& initialized, std::vector<float> const& gains)
    {
      for (auto const gain : gains)
      {
        recordGain(maxGain, minGain, initialized, gain);
      }
    };

    auto cls = VolumeClassification{};
    bool softwareGainRecorded = false;
    bool unclassifiedGainRecorded = false;

    bool const softNotUnity = hasSoftVolumes && std::ranges::any_of(softVolumes, isNotUnity);

    if (softNotUnity)
    {
      cls.softwareNotUnity = true;
    }

    if (hasSoftVolumes && !softVolumes.empty())
    {
      recordGains(cls.maxSoftwareGain, cls.minSoftwareGain, softwareGainRecorded, softVolumes);
    }

    bool const channelNotUnity = hasChannelVolumes && std::ranges::any_of(channelVolumes, isNotUnity);
    bool const scalarNotUnity = hasVolume && isNotUnity(volume);
    // The scalar volume and the per-channel volumes are independent properties carrying independent
    // hardware flags, so each one decides its own provenance. A hardware-flagged property never
    // contributes a magnitude to a software or unclassified range, even when the other property is
    // ambiguous and makes the whole classification mixed.
    bool const channelIsAmbiguous = channelNotUnity && !channelVolumesAreHardware;
    bool const scalarIsAmbiguous = scalarNotUnity && !volumeIsHardware;
    bool const hardwareNotUnity =
      (channelNotUnity && channelVolumesAreHardware) || (scalarNotUnity && volumeIsHardware);
    bool const ambiguousNotUnity = channelIsAmbiguous || scalarIsAmbiguous;

    if (hardwareNotUnity)
    {
      cls.hardwareNotUnity = true;
    }

    auto recordAmbiguousGains = [this, &recordGain, &recordGains, channelIsAmbiguous, scalarIsAmbiguous](
                                  float& maxGain, float& minGain, bool& initialized)
    {
      if (channelIsAmbiguous)
      {
        recordGains(maxGain, minGain, initialized, channelVolumes);
      }

      if (scalarIsAmbiguous)
      {
        recordGain(maxGain, minGain, initialized, volume);
      }
    };

    if (scope == VolumeClassificationScope::Stream && ambiguousNotUnity)
    {
      cls.softwareNotUnity = true;
      recordAmbiguousGains(cls.maxSoftwareGain, cls.minSoftwareGain, softwareGainRecorded);
    }
    else if (ambiguousNotUnity && !softNotUnity)
    {
      cls.unclassifiedNotUnity = true;
      recordAmbiguousGains(cls.maxUnclassifiedGain, cls.minUnclassifiedGain, unclassifiedGainRecorded);
    }

    return cls;
  }
} // namespace ao::audio::backend::detail
