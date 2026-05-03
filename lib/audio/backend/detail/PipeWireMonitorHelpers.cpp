// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/backend/detail/PipeWireMonitorHelpers.h>
#include <ao/audio/backend/detail/PipeWireShared.h>
#include <ao/utility/ByteView.h>

extern "C"
{
#include <pipewire/keys.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/utils/dict.h>
}

#include <algorithm>
#include <array>
#include <format>
#include <ranges>

namespace ao::audio::backend::detail
{
  bool isSinkMediaClass(std::string_view mediaClass) noexcept
  {
    if (!mediaClass.contains("Audio")) return false;
    return mediaClass.ends_with("/Sink") || mediaClass.ends_with("/Duplex");
  }

  std::string lookupProperty(::spa_dict const* props, char const* key) noexcept
  {
    auto const* value = props != nullptr ? ::spa_dict_lookup(props, key) : nullptr;
    return value != nullptr ? std::string{value} : std::string{};
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

    if (auto const serial = parseUintProperty(::spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL)))
    {
      record.objectSerial = serial;
    }

    if (auto const id = parseUintProperty(::spa_dict_lookup(props, "node.driver-id")))
    {
      record.driverId = id;
    }
    else if (auto const id = parseUintProperty(::spa_dict_lookup(props, "node.driver")))
    {
      record.driverId = id;
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
      auto const n_vals = SPA_POD_CHOICE_N_VALUES(choice);
      auto const type = SPA_POD_CHOICE_VALUE_TYPE(choice);

      if (n_vals == 0 || type != SPA_TYPE_Int)
      {
        return;
      }

      auto const* vals = static_cast<std::int32_t const*>(SPA_POD_CHOICE_VALUES(choice));
      auto const choice_type = SPA_POD_CHOICE_TYPE(choice);

      if (choice_type == SPA_CHOICE_Enum || choice_type == SPA_CHOICE_None)
      {
        for (std::uint32_t i = 0; i < n_vals; ++i)
        {
          addUnique(output, static_cast<std::uint32_t>(vals[i]));
        }
      }
      else if (choice_type == SPA_CHOICE_Range)
      {
        auto const min = (n_vals > 1) ? vals[1] : vals[0];
        auto const max = (n_vals > 2) ? vals[2] : min;

        static constexpr auto commonRates =
          std::array<std::uint32_t, 8>{44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};

        for (auto rate : commonRates)
        {
          if (rate >= static_cast<std::uint32_t>(min) && rate <= static_cast<std::uint32_t>(max))
          {
            addUnique(output, rate);
          }
        }
        addUnique(output, static_cast<std::uint32_t>(vals[0]));
      }
    }

    void collectIntValues(::spa_pod const* pod, std::vector<std::uint32_t>& output)
    {
      if (pod == nullptr) return;
      if (::spa_pod_is_int(pod) != 0)
      {
        std::int32_t val = 0;
        if (::spa_pod_get_int(pod, &val) == 0) addUnique(output, static_cast<std::uint32_t>(val));
      }
      else if (::spa_pod_is_choice(pod) != 0)
      {
        auto const podSpan = ao::utility::bytes::view(pod, pod->size + sizeof(::spa_pod));
        auto const* choice = ao::utility::layout::view<::spa_pod_choice>(podSpan);
        processChoiceIntValues(choice, output);
      }
    }

    void collectIdValues(::spa_pod const* pod, std::vector<std::uint32_t>& output)
    {
      if (pod == nullptr) return;
      if (::spa_pod_is_id(pod) != 0)
      {
        std::uint32_t val = 0;
        if (::spa_pod_get_id(pod, &val) == 0) addUnique(output, val);
      }
      else if (::spa_pod_is_choice(pod) != 0)
      {
        auto const podSpan = ao::utility::bytes::view(pod, pod->size + sizeof(::spa_pod));
        auto const* choice = ao::utility::layout::view<::spa_pod_choice>(podSpan);
        auto const n_vals = SPA_POD_CHOICE_N_VALUES(choice);
        if (n_vals == 0 || SPA_POD_CHOICE_VALUE_TYPE(choice) != SPA_TYPE_Id) return;
        auto const* vals = static_cast<std::uint32_t const*>(SPA_POD_CHOICE_VALUES(choice));
        auto const choice_type = SPA_POD_CHOICE_TYPE(choice);
        if (choice_type == SPA_CHOICE_Enum || choice_type == SPA_CHOICE_None)
        {
          for (std::uint32_t i = 0; i < n_vals; ++i) addUnique(output, vals[i]);
        }
      }
    }
  }

  void parseEnumFormat(::spa_pod const* param, DeviceCapabilities& caps)
  {
    if (param == nullptr || ::spa_pod_is_object(param) == 0) return;
    ::spa_pod_prop const* prop = nullptr;
    auto const podSpan = ao::utility::bytes::view(param, param->size + sizeof(::spa_pod));
    auto const* obj = ao::utility::layout::view<::spa_pod_object>(podSpan);

    SPA_POD_OBJECT_FOREACH(obj, prop)
    {
      if (prop->key == SPA_FORMAT_AUDIO_format)
      {
        std::vector<std::uint32_t> formats;
        collectIdValues(&prop->value, formats);
        for (auto const format : formats)
        {
          if (auto const optCapability = sampleFormatCapabilityFromSpaFormat(format))
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
        std::vector<std::uint32_t> channels;
        collectIntValues(&prop->value, channels);
        for (auto const channelCount : channels)
        {
          if (channelCount > 0 && channelCount <= 255)
          {
            auto const c8 = static_cast<std::uint8_t>(channelCount);

            if (!std::ranges::contains(caps.channelCounts, c8))
            {
              caps.channelCounts.push_back(c8);
            }
          }
        }
      }
    }
  }

  bool copyFloatArray(::spa_pod const& pod, std::vector<float>& output)
  {
    auto values = std::array<float, 16>{};
    auto const count = ::spa_pod_copy_array(&pod, SPA_TYPE_Float, values.data(), values.size());

    if (count == 0)
    {
      return false;
    }

    output.assign_range(values | std::views::take(count));
    return true;
  }

  void mergeSinkProps(SinkProps& sinkProps, ::spa_pod const* param)
  {
    if (param == nullptr)
    {
      return;
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_volume))
    {
      float val = 0.0F;
      if (::spa_pod_get_float(&prop->value, &val) == 0)
      {
        sinkProps.volume = val;
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_mute))
    {
      bool val = false;
      if (::spa_pod_get_bool(&prop->value, &val) == 0)
      {
        sinkProps.isMuted = val;
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes))
    {
      copyFloatArray(prop->value, sinkProps.channelVolumes);
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_softMute))
    {
      bool val = false;
      if (::spa_pod_get_bool(&prop->value, &val) == 0)
      {
        sinkProps.isSoftMuted = val;
      }
    }

    if (auto const* prop = ::spa_pod_find_prop(param, nullptr, SPA_PROP_softVolumes))
    {
      copyFloatArray(prop->value, sinkProps.softVolumes);
    }
  }

  bool SinkProps::isUnity() const noexcept
  {
    auto const checkUnity = [](float val) { return std::abs(val - 1.0F) < 1e-4F; }; // NOLINT(readability-magic-numbers)

    if (!checkUnity(volume))
    {
      return false;
    }

    if (!channelVolumes.empty() && !std::ranges::all_of(channelVolumes, checkUnity))
    {
      return false;
    }

    if (!softVolumes.empty() && !std::ranges::all_of(softVolumes, checkUnity))
    {
      return false;
    }

    return true;
  }
} // namespace ao::audio::backend::detail
