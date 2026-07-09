// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Device.h>
#include <ao/audio/Format.h>
#include <ao/audio/backend/detail/AudioBackendFormatSupport.h>
#include <ao/audio/backend/detail/PipeWireFormatParsing.h>
#include <ao/audio/backend/detail/PipeWireMonitorParsing.h>
#include <ao/utility/ByteView.h>

extern "C"
{
#include <pipewire/keys.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/body.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/utils/dict.h>
#include <spa/utils/type.h>
}

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
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

  std::optional<SampleFormatCapability> sampleFormatCapabilityFromSpaFormat(std::uint32_t spaFmt) noexcept
  {
    switch (spaFmt)
    {
      case SPA_AUDIO_FORMAT_S16_LE:
      case SPA_AUDIO_FORMAT_S16_BE: return SampleFormatCapability{.bitDepth = 16, .validBits = 16, .isFloat = false};
      case SPA_AUDIO_FORMAT_S24_LE:
      case SPA_AUDIO_FORMAT_S24_BE: return SampleFormatCapability{.bitDepth = 24, .validBits = 24, .isFloat = false};
      case SPA_AUDIO_FORMAT_S24_32_LE:
      case SPA_AUDIO_FORMAT_S24_32_BE: return SampleFormatCapability{.bitDepth = 32, .validBits = 24, .isFloat = false};
      case SPA_AUDIO_FORMAT_S32_LE:
      case SPA_AUDIO_FORMAT_S32_BE: return SampleFormatCapability{.bitDepth = 32, .validBits = 32, .isFloat = false};
      case SPA_AUDIO_FORMAT_F32_LE:
      case SPA_AUDIO_FORMAT_F32_BE: return SampleFormatCapability{.bitDepth = 32, .validBits = 32, .isFloat = true};
      case SPA_AUDIO_FORMAT_F64_LE:
      case SPA_AUDIO_FORMAT_F64_BE: return SampleFormatCapability{.bitDepth = 64, .validBits = 64, .isFloat = true};
      default: return std::nullopt;
    }
  }

  namespace

  {
    void addUnique(std::vector<std::uint32_t>& output, std::uint32_t value)
    {
      if (!std::ranges::contains(output, value))
      {
        output.push_back(value);
      }
    }

    void processChoiceIntValues(::spa_pod_choice const* choice, std::vector<std::uint32_t>& output)
    {
      auto const nVals = SPA_POD_CHOICE_N_VALUES(choice);

      if (nVals == 0 || SPA_POD_CHOICE_VALUE_TYPE(choice) != SPA_TYPE_Int)
      {
        return;
      }

      auto const* vals = static_cast<std::int32_t const*>(SPA_POD_CHOICE_VALUES(choice));

      if (auto const choiceType = SPA_POD_CHOICE_TYPE(choice);
          choiceType == SPA_CHOICE_Enum || choiceType == SPA_CHOICE_None)
      {
        for (std::uint32_t i = 0; i < nVals; ++i)
        {
          addUnique(output, static_cast<std::uint32_t>(vals[i]));
        }
      }
      else if (choiceType == SPA_CHOICE_Range)
      {
        auto const min = (nVals > 1) ? vals[1] : vals[0];
        auto const max = (nVals > 2) ? vals[2] : min;

        static constexpr auto kCommonRates =
          std::array<std::uint32_t, 8>{44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};

        for (auto rate : kCommonRates)
        {
          if (std::cmp_greater_equal(rate, min) && std::cmp_less_equal(rate, max))
          {
            addUnique(output, rate);
          }
        }

        addUnique(output, static_cast<std::uint32_t>(vals[0]));
      }
    }

    void collectIntValues(::spa_pod const* pod, std::vector<std::uint32_t>& output)
    {
      if (pod == nullptr)
      {
        return;
      }

      if (::spa_pod_is_int(pod) != 0)
      {
        if (std::int32_t val = 0; ::spa_pod_get_int(pod, &val) == 0)
        {
          addUnique(output, static_cast<std::uint32_t>(val));
        }
      }
      else if (::spa_pod_is_choice(pod) != 0)
      {
        auto const podSpan = utility::bytes::view(pod, pod->size + sizeof(::spa_pod));
        auto const* choice = utility::layout::view<::spa_pod_choice>(podSpan);
        processChoiceIntValues(choice, output);
      }
    }

    void collectIdValues(::spa_pod const* pod, std::vector<std::uint32_t>& output)
    {
      if (pod == nullptr)
      {
        return;
      }

      if (::spa_pod_is_id(pod) != 0)
      {
        if (std::uint32_t val = 0; ::spa_pod_get_id(pod, &val) == 0)
        {
          addUnique(output, val);
        }
      }
      else if (::spa_pod_is_choice(pod) != 0)
      {
        auto const podSpan = utility::bytes::view(pod, pod->size + sizeof(::spa_pod));
        auto const* choice = utility::layout::view<::spa_pod_choice>(podSpan);
        auto const nVals = SPA_POD_CHOICE_N_VALUES(choice);

        if (nVals == 0 || SPA_POD_CHOICE_VALUE_TYPE(choice) != SPA_TYPE_Id)
        {
          return;
        }

        auto const* vals = static_cast<std::uint32_t const*>(SPA_POD_CHOICE_VALUES(choice));

        if (auto const choiceType = SPA_POD_CHOICE_TYPE(choice);
            choiceType == SPA_CHOICE_Enum || choiceType == SPA_CHOICE_None)
        {
          for (std::uint32_t i = 0; i < nVals; ++i)
          {
            addUnique(output, vals[i]);
          }
        }
      }
    }
  } // namespace

  void parseEnumFormat(::spa_pod const* param, DeviceCapabilities& caps)
  {
    if (param == nullptr || ::spa_pod_is_object(param) == 0)
    {
      return;
    }

    ::spa_pod_prop const* prop = nullptr;
    auto const podSpan = utility::bytes::view(param, param->size + sizeof(::spa_pod));
    auto const* obj = utility::layout::view<::spa_pod_object>(podSpan);

    SPA_POD_OBJECT_FOREACH(obj, prop)
    {
      if (prop->key == SPA_FORMAT_AUDIO_format)
      {
        auto formats = std::vector<std::uint32_t>{};
        collectIdValues(&prop->value, formats);

        for (auto const format : formats)
        {
          if (auto const optCapability = sampleFormatCapabilityFromSpaFormat(format); optCapability)
          {
            addSampleFormatCapability(caps, *optCapability);
          }
        }
      }
      else if (prop->key == SPA_FORMAT_AUDIO_rate)
      {
        collectIntValues(&prop->value, caps.sampleRates);
      }
      else if (prop->key == SPA_FORMAT_AUDIO_channels)
      {
        auto channels = std::vector<std::uint32_t>{};
        collectIntValues(&prop->value, channels);

        for (auto const channelCount : channels)
        {
          static constexpr std::uint32_t kMaxChannelCount = 255;

          if (channelCount > 0 && channelCount <= kMaxChannelCount)
          {
            if (auto const c8 = static_cast<std::uint8_t>(channelCount); !std::ranges::contains(caps.channelCounts, c8))
            {
              caps.channelCounts.push_back(c8);
            }
          }
        }
      }
    }
  }

  std::optional<Format> currentFormatFromNodeParam(std::uint32_t const paramId, ::spa_pod const* const param) noexcept
  {
    if (paramId != SPA_PARAM_Format)
    {
      return std::nullopt;
    }

    return parseRawStreamFormat(param);
  }

  void updateCurrentFormatFromNodeParam(std::unordered_map<std::uint32_t, Format>& nodeFormatMap,
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
    bool copyFloatArray(::spa_pod const& pod, std::vector<float>& output)
    {
      auto values = std::array<float, 16>{};
      auto const count = ::spa_pod_copy_array_full(&pod, SPA_TYPE_Float, sizeof(float), values.data(), values.size());

      if (count == 0)
      {
        return false;
      }

      output.assign(values.begin(), values.begin() + count);
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
      if (copyFloatArray(prop->value, sinkProps.channelVolumes))
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
      if (copyFloatArray(prop->value, sinkProps.softVolumes))
      {
        sinkProps.hasSoftVolumes = true;
      }
    }
  }

  SinkProps::VolumeClassification SinkProps::classifyVolume(VolumeClassificationContext const context) const noexcept
  {
    static constexpr float kUnityEpsilon = 1e-4F;
    auto const isNotUnity = [](float val) { return std::abs(val - 1.0F) >= kUnityEpsilon; };
    auto recordSoftwareGain = [](VolumeClassification& cls, float const gain)
    {
      if (cls.maxSoftwareGain == 0.0F || gain > cls.maxSoftwareGain)
      {
        cls.maxSoftwareGain = gain;
      }

      if (cls.minSoftwareGain == 0.0F || gain < cls.minSoftwareGain)
      {
        cls.minSoftwareGain = gain;
      }
    };
    auto recordSoftwareGains = [&recordSoftwareGain](VolumeClassification& cls, std::vector<float> const& gains)
    {
      for (auto const gain : gains)
      {
        recordSoftwareGain(cls, gain);
      }
    };

    auto cls = VolumeClassification{};

    bool const softNotUnity = hasSoftVolumes && std::ranges::any_of(softVolumes, isNotUnity);

    if (softNotUnity)
    {
      cls.softwareNotUnity = true;
    }

    if (hasSoftVolumes && !softVolumes.empty())
    {
      recordSoftwareGains(cls, softVolumes);
    }

    bool const channelNotUnity = hasChannelVolumes && std::ranges::any_of(channelVolumes, isNotUnity);
    bool const scalarNotUnity = hasVolume && isNotUnity(volume);
    bool const hardwareNotUnity =
      (channelNotUnity && channelVolumesAreHardware) || (scalarNotUnity && volumeIsHardware);
    bool const ambiguousNotUnity =
      (channelNotUnity && !channelVolumesAreHardware) || (scalarNotUnity && !volumeIsHardware);

    if (hardwareNotUnity)
    {
      cls.hardwareNotUnity = true;
    }

    if (context == VolumeClassificationContext::Stream && ambiguousNotUnity)
    {
      cls.softwareNotUnity = true;

      if (channelNotUnity)
      {
        recordSoftwareGains(cls, channelVolumes);
      }

      if (scalarNotUnity)
      {
        recordSoftwareGain(cls, volume);
      }
    }
    else if (ambiguousNotUnity && !softNotUnity)
    {
      cls.unclassifiedNotUnity = true;
    }

    return cls;
  }
} // namespace ao::audio::backend::detail
