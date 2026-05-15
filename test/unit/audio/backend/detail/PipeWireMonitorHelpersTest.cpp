// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/backend/detail/AudioBackendShared.h>
#include <ao/audio/backend/detail/PipeWireMonitorHelpers.h>

#include <catch2/catch_test_macros.hpp>

extern "C"
{
#include <pipewire/keys.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/body.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <spa/pod/vararg.h>
#include <spa/utils/dict.h>
#include <spa/utils/type.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-extensions"
#endif

namespace ao::audio::backend::detail::test
{
  TEST_CASE("PipeWireMonitorHelpers - Property Parsing", "[audio][pipewire][monitor]")
  {
    SECTION("isSinkMediaClass")
    {
      CHECK(isSinkMediaClass("Audio/Sink"));
      CHECK(isSinkMediaClass("Audio/Duplex"));
      CHECK(isSinkMediaClass("Stream/Output/Audio/Sink"));
      CHECK_FALSE(isSinkMediaClass("Audio/Source"));
      CHECK_FALSE(isSinkMediaClass("Video/Sink"));
    }

    SECTION("lookupProperty")
    {
      struct spa_dict_item items[] = {SPA_DICT_ITEM_INIT("key1", "val1"), SPA_DICT_ITEM_INIT("key2", "val2")}; // NOLINT
      struct spa_dict dict = SPA_DICT_INIT(items, 2);                                                          // NOLINT

      CHECK(lookupProperty(&dict, "key1") == "val1");
      CHECK(lookupProperty(&dict, "key2") == "val2");
      CHECK(lookupProperty(&dict, "key3").empty());
      CHECK(lookupProperty(nullptr, "key1").empty());
    }

    SECTION("parseNodeRecord")
    {
      struct spa_dict_item items[] = {SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_CLASS, "Audio/Sink"), // NOLINT
                                      SPA_DICT_ITEM_INIT(PW_KEY_NODE_NAME, "test-node"),    // NOLINT
                                      SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_SERIAL, "1234"),     // NOLINT
                                      SPA_DICT_ITEM_INIT("node.driver-id", "5678")};        // NOLINT
      struct spa_dict dict = SPA_DICT_INIT(items, 4);                                       // NOLINT

      auto record = parseNodeRecord(1, &dict);
      CHECK(record.version == 1);
      CHECK(record.mediaClass == "Audio/Sink");
      CHECK(record.nodeName == "test-node");
      CHECK(record.optObjectSerial == 1234);
      CHECK(record.optDriverId == 5678);
    }
  }

  TEST_CASE("PipeWireMonitorHelpers - Format Capabilities", "[audio][pipewire][monitor]")
  {
    SECTION("sampleFormatCapabilityFromSpaFormat")
    {
      auto optCap16 = sampleFormatCapabilityFromSpaFormat(SPA_AUDIO_FORMAT_S16_LE);
      REQUIRE(optCap16);
      CHECK(optCap16->bitDepth == 16);
      CHECK(optCap16->isFloat == false);

      auto optCapF32 = sampleFormatCapabilityFromSpaFormat(SPA_AUDIO_FORMAT_F32_LE);
      REQUIRE(optCapF32);
      CHECK(optCapF32->bitDepth == 32);
      CHECK(optCapF32->isFloat == true);

      CHECK_FALSE(sampleFormatCapabilityFromSpaFormat(SPA_AUDIO_FORMAT_UNKNOWN));
    }

    SECTION("addSampleFormatCapability")
    {
      auto caps = DeviceCapabilities{};
      auto cap = SampleFormatCapability{.bitDepth = 32, .validBits = 24, .isFloat = false};

      addSampleFormatCapability(caps, cap);
      CHECK(caps.sampleFormats.size() == 1);
      CHECK(caps.bitDepths.empty()); // Not added because validBits != bitDepth

      auto cap16 = SampleFormatCapability{.bitDepth = 16, .validBits = 16, .isFloat = false};
      addSampleFormatCapability(caps, cap16);
      CHECK(caps.sampleFormats.size() == 2);
      CHECK(caps.bitDepths == std::vector<uint8_t>{16});
    }
  }

  TEST_CASE("PipeWireMonitorHelpers - SPA Pod Parsing", "[audio][pipewire][monitor]")
  {
    auto buffer = std::array<std::byte, 1024>{};
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size()); // NOLINT

    SECTION("parseEnumFormat - Sample Rates and Channels")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      ::spa_pod_builder_add(&b,
                            SPA_FORMAT_mediaType,
                            SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                            SPA_FORMAT_mediaSubtype,
                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_AUDIO_format,
                            SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
                            SPA_FORMAT_AUDIO_channels,
                            SPA_POD_Int(2),
                            0);

      // Add rate as a range
      ::spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_rate, 0);
      auto f2 = ::spa_pod_frame{};
      ::spa_pod_builder_push_choice(&b, &f2, SPA_CHOICE_Range, 0);
      ::spa_pod_builder_int(&b, 48000);  // default
      ::spa_pod_builder_int(&b, 44100);  // min
      ::spa_pod_builder_int(&b, 192000); // max
      ::spa_pod_builder_pop(&b, &f2);

      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto caps = DeviceCapabilities{};
      parseEnumFormat(pod, caps);

      CHECK_FALSE(caps.sampleRates.empty());
      CHECK(std::ranges::contains(caps.sampleRates, 48000U));
      CHECK(std::ranges::contains(caps.sampleRates, 44100U));
      CHECK(std::ranges::contains(caps.sampleRates, 192000U));
      REQUIRE(caps.channelCounts.size() == 1);
      CHECK(caps.channelCounts[0] == 2);
      REQUIRE(caps.bitDepths.size() == 1);
      CHECK(caps.bitDepths[0] == 16);
    }

    SECTION("parseEnumFormat - Discrete Sample Rates")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      ::spa_pod_builder_add(&b,
                            SPA_FORMAT_mediaType,
                            SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                            SPA_FORMAT_mediaSubtype,
                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_AUDIO_format,
                            SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
                            SPA_FORMAT_AUDIO_channels,
                            SPA_POD_Int(2),
                            0);

      // Add rate as an enum
      ::spa_pod_builder_prop(&b, SPA_FORMAT_AUDIO_rate, 0);
      auto f2 = ::spa_pod_frame{};
      ::spa_pod_builder_push_choice(&b, &f2, SPA_CHOICE_Enum, 0);
      ::spa_pod_builder_int(&b, 44100); // default
      ::spa_pod_builder_int(&b, 44100); // choice 1
      ::spa_pod_builder_int(&b, 48000); // choice 2
      ::spa_pod_builder_pop(&b, &f2);

      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto caps = DeviceCapabilities{};
      parseEnumFormat(pod, caps);

      CHECK(caps.sampleRates.size() == 2);
      CHECK(std::ranges::contains(caps.sampleRates, 44100U));
      CHECK(std::ranges::contains(caps.sampleRates, 44100U));
      CHECK(std::ranges::contains(caps.sampleRates, 48000U));
    }

    SECTION("mergeSinkProps - volume and mute")
    {
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_add(&b, SPA_PROP_volume, SPA_POD_Float(0.5F), SPA_PROP_mute, SPA_POD_Bool(true), 0);
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);
      CHECK(std::abs(props.volume - 0.5F) < 1e-4F);
      CHECK(props.isMuted == true);
      CHECK_FALSE(props.isUnity());
    }

    SECTION("mergeSinkProps - channel volumes")
    {
      auto const vols = std::array{1.0F, 0.8F};
      auto f = ::spa_pod_frame{};
      ::spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
      ::spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
      ::spa_pod_builder_array(
        &b, sizeof(float), SPA_TYPE_Float, vols.size(), vols.data()); // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
      auto* pod = static_cast<::spa_pod*>(::spa_pod_builder_pop(&b, &f));

      auto props = SinkProps{};
      mergeSinkProps(props, pod);
      REQUIRE(props.channelVolumes.size() == 2);
      CHECK(props.channelVolumes[0] == 1.0F);
      CHECK(props.channelVolumes[1] == 0.8F);
      CHECK_FALSE(props.isUnity());
    }

    SECTION("SinkProps::isUnity - corner cases")
    {
      auto props = SinkProps{};
      CHECK(props.isUnity()); // Default is 1.0

      props.volume = 0.99999F;
      CHECK(props.isUnity()); // Within tolerance

      props.volume = 0.999F;
      CHECK_FALSE(props.isUnity());

      props.volume = 1.0F;
      props.channelVolumes = {1.0F, 1.0F, 0.99999F};
      CHECK(props.isUnity());

      props.channelVolumes[2] = 0.99F;
      CHECK_FALSE(props.isUnity());

      props.channelVolumes.clear();
      props.softVolumes = {1.0F, 0.5F};
      CHECK_FALSE(props.isUnity());
    }
  }

#ifdef __clang__
#pragma clang diagnostic pop
#endif
} // namespace ao::audio::backend::detail::test
